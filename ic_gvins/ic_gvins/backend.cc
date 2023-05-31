#include "backend.h"
#if 0
#include "vslam/ceres_helper/reprojection_.hpp"
#include "vslam/ceres_helper/se3_parameterization.hpp"
#else
#include "factors/reprojection_factor.h"
#include "factors/pose_parameterization.h"
#endif
// #include "vslam/config.h"
#include "tracking/feature.h"
#include "tracking/map.h"
#include "tracking/mappoint.h"
// #include "tracking/utility.h"

struct pose_para
{
    double pose[7];
};

Backend::Backend()
{
    // min_reprojerr_ = Config::Get<double>("min_reprojection_error");

    backend_running_.store(true);
    backend_thread_ = std::thread(std::bind(&Backend::BackendLoop, this));
}

void Backend::UpdateMap()
{
    std::unique_lock<std::mutex> lock(data_mutex_);
    map_update_.notify_one();
}

void Backend::Stop()
{
    backend_running_.store(false);
    map_update_.notify_one();
    backend_thread_.join();
}

void Backend::BackendLoop()
{
    while (backend_running_.load())
    {
        auto t1 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(data_mutex_);
        map_update_.wait(lock);
        Optimize();
        auto t2 = std::chrono::steady_clock::now();
        auto time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
        LOG(INFO) << "backend time : " << time_used.count();
    }
}

void Backend::Optimize()
{

#if 1
    if (map_->landmarks().empty()) {
        return;
    }
    Map::KeyFrames active_kfs = map_->keyframes();
    Map::LandMarks active_landmarks = map_->landmarks();

    ceres::Problem problem;
    ceres::LossFunction *loss_function = new ceres::HuberLoss(1.0);

    std::unordered_map<int, struct pose_para> keyframePoses_data;
    for (auto &keyframe : active_kfs)
    {   
        Pose pose = keyframe.second->pose();
        pose_para data;
        memcpy(data.pose, pose.t.data(), sizeof(double) * 3);
        memcpy(data.pose+3, Rotation::matrix2quaternion(pose.R).coeffs().data(), sizeof(double) * 4);

        keyframePoses_data.insert({keyframe.first, data});
        double *para = keyframePoses_data[keyframe.first].pose;

        ceres::LocalParameterization *parameterization = new (PoseParameterization);
        problem.AddParameterBlock(para, 7, parameterization);
    }


    std::unordered_map<ulong, double> invdepthlist_;
    for (const auto &landmark : active_landmarks) {
        const auto &mappoint = landmark.second;
        if (!mappoint || mappoint->isOutlier()) {
            continue;
        }

        if (invdepthlist_.find(mappoint->id()) == invdepthlist_.end()) {
            auto frame = mappoint->referenceFrame();
            if (!frame || !(active_kfs.find(frame->keyFrameId()) != active_kfs.end())) { // Original : !map_->isKeyFrameInMap(frame)) {
                continue;
            }

            double depth         = mappoint->depth();
            double inverse_depth = 1.0 / depth;

            // 确保深度数值有效
            // For valid mappoints
            if (std::isnan(inverse_depth)) {
                mappoint->setOutlier(true);
                // LOGE << "Mappoint " << mappoint->id() << " is wrong with depth " << depth << " type "
                //      << mappoint->mapPointType();
                continue;
            }

            invdepthlist_[mappoint->id()] = inverse_depth;
            problem.AddParameterBlock(&invdepthlist_[mappoint->id()], 1);

            mappoint->addOptimizedTimes();
        }
    }

 // 外参
    // Extrinsic parameters
    extrinsic_[0] = 0;
    extrinsic_[1] = 0;
    extrinsic_[2] = 0;

    Quaterniond qic = Rotation::matrix2quaternion(Matrix3d::Identity());
    qic.normalize();
    extrinsic_[3] = qic.x();
    extrinsic_[4] = qic.y();
    extrinsic_[5] = qic.z();
    extrinsic_[6] = qic.w();

    ceres::LocalParameterization *parameterization = new (PoseParameterization);
    problem.AddParameterBlock(extrinsic_, 7, parameterization);

    problem.SetParameterBlockConstant(extrinsic_);

    // 时间延时
    // Time delay
    extrinsic_[7] = 0;
    problem.AddParameterBlock(&extrinsic_[7], 1);
    problem.SetParameterBlockConstant(&extrinsic_[7]);

    for (const auto &landmark : active_landmarks) {
        const auto &mappoint = landmark.second;
        if (!mappoint || mappoint->isOutlier()) {
            continue;
        }

        if (invdepthlist_.find(mappoint->id()) == invdepthlist_.end()) {
            continue;
        }

        auto ref_frame = mappoint->referenceFrame();
        if (!(active_kfs.find(ref_frame->keyFrameId()) != active_kfs.end())){  // Original !map_->isKeyFrameInMap(ref_frame)) {
            continue;
        }

        auto ref_frame_pc      = camera_->pixel2cam(mappoint->referenceKeypoint());
        // size_t ref_frame_index = getStateDataIndex(ref_frame->stamp());
        // if (ref_frame_index < 0) {
        //     continue;
        // }

        double *invdepth = &invdepthlist_[mappoint->id()];
        if (*invdepth == 0) {
            *invdepth = 1.0 / MapPoint::DEFAULT_DEPTH;
        }

        auto ref_feature = ref_frame->features().find(mappoint->id())->second;

        auto observations = mappoint->observations();
        for (auto &observation : observations) {
            auto obs_feature = observation.lock();
            if (!obs_feature || obs_feature->isOutlier()) {
                continue;
            }
            auto obs_frame = obs_feature->getFrame();
            if (!obs_frame || !obs_frame->isKeyFrame() || !(active_kfs.find(obs_frame->keyFrameId()) != active_kfs.end()) /* original : !map_->isKeyFrameInMap(obs_frame) */ || (obs_frame == ref_frame)) {
                continue;
            }

            auto obs_frame_pc      = camera_->pixel2cam(obs_feature->keyPoint());
            // size_t obs_frame_index = getStateDataIndex(obs_frame->stamp());

            // if ((obs_frame_index < 0) || (ref_frame_index == obs_frame_index)) {
            //     // LOGE << "Wrong matched mapoint keyframes " << Logging::doubleData(ref_frame->stamp()) << " with "
            //     //      << Logging::doubleData(obs_frame->stamp());
            //     continue;
            // }

            auto factor = new ReprojectionFactor(ref_frame_pc, obs_frame_pc, ref_feature->velocityInPixel(),
                                                 obs_feature->velocityInPixel(), ref_frame->timeDelay(),
                                                 obs_frame->timeDelay(), 1.5);
            auto residual_block_id =
                problem.AddResidualBlock(factor, loss_function, keyframePoses_data[ref_frame->keyFrameId()].pose,
                                         keyframePoses_data[obs_frame->keyFrameId()].pose, extrinsic_, invdepth, &extrinsic_[7]);
            // residual_ids.push_back(residual_block_id);
        }
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    options.max_num_iterations = 20;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    // LOG(INFO) << summary.FullReport();

    
    // Update map from optimzed results
    for (auto &keyframePose: keyframePoses_data)
    {
        Pose pose;
        Quaterniond q;

        memcpy(pose.t.data(), keyframePose.second.pose, sizeof(double) * 3);
        memcpy(q.coeffs().data(), keyframePose.second.pose+3, sizeof(double) * 4);
        pose.R = Rotation::quaternion2matrix(q);
        active_kfs[keyframePose.first]->setPose(pose);
    }

    for (const auto &landmark : active_landmarks) {
        const auto &mappoint = landmark.second;
        if (!mappoint || mappoint->isOutlier()) {
            continue;
        }

        auto frame = mappoint->referenceFrame();
        if (!frame || !(active_kfs.find(frame->keyFrameId()) != active_kfs.end())){ // orignal !map_->isKeyFrameInMap(frame)) {
            continue;
        }

        if (invdepthlist_.find(mappoint->id()) == invdepthlist_.end()) {
            continue;
        }

        double invdepth = invdepthlist_[mappoint->id()];
        double depth    = 1.0 / invdepth;

        auto pc0      = camera_->pixel2cam(mappoint->referenceKeypoint());
        Vector3d pc00 = {pc0.x(), pc0.y(), 1.0};
        pc00 *= depth;

        mappoint->pos() = camera_->cam2world(pc00, mappoint->referenceFrame()->pose());
        mappoint->updateDepth(depth);
    }

#else
    // Map::ParamsType para_kfs = map_->GetPoseParams();
    // Map::ParamsType para_landmarks = map_->GetPointParams();
    Map::KeyFrames active_kfs = map_->keyframes();
    Map::LandMarks active_landmarks = map_->landmarks();

    ceres::Problem problem;
    ceres::LossFunction *loss_function = new ceres::HuberLoss(1.0);

    ceres::LocalParameterization *local_parameterization = new SE3Parameterization();
    std::unordered_map<int, SE3> keyframePoses;
    for (auto &keyframe : active_kfs)
    {   
        Mat33 R = keyframe.second->pose().R;
        R = R + 0.5 * (Mat33::Identity() - R * R.transpose()) * R; // Todo
        SE3 pose(R, keyframe.second->pose().t);
        keyframePoses.insert({keyframe.first, pose});
        double *para = keyframePoses[keyframe.first].data();
        problem.AddParameterBlock(para, SE3::num_parameters, local_parameterization);
    }

    std::unordered_map<int, Vec3> mapPoints;
    for (auto &landmark : active_landmarks)
    {
        if (landmark.second->isOutlier())
            continue;
        mapPoints.insert({landmark.first, landmark.second->pos()});
        double *para = mapPoints[landmark.first].data();
        // double *para = landmark.second->Pos().data();
        // double *para = para_landmarks[landmark.first];
        problem.AddParameterBlock(para, 3);
        auto observations = landmark.second->observations();
        for (auto &obs : observations)
        {
            if (obs.lock() == nullptr)
                continue;
            auto feature = obs.lock();
            if (feature->isOutlier() || feature->getFrame() == nullptr)
                continue;
            auto frame = feature->getFrame();
            auto iter = active_kfs.find(frame->keyFrameId());
            if (iter == active_kfs.end())
                continue;
            auto keyframe = *iter;

            ceres::CostFunction *cost_function;
            cost_function = new ReprojectionError(toVec2(feature->keyPoint()), camera_);
            // problem.AddResidualBlock(cost_function, loss_function, feature->getFrame()->Pose().data(), para);
            problem.AddResidualBlock(cost_function, loss_function, keyframePoses[keyframe.first].data(), para);
        }
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    options.max_num_iterations = 20;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    // LOG(INFO) << summary.FullReport();


    // // Update map from optimzed results
    for (auto &keyframePose: keyframePoses)
    {
        Pose pose({keyframePose.second.rotationMatrix(), keyframePose.second.translation()});
        active_kfs[keyframePose.first]->setPose(pose);
    }
    for (auto &mp : mapPoints)
    {
        active_landmarks[mp.first]->pos() = (mp.second);
    }

    // // reject outliers
    // int cnt_outlier = 0, cnt_inlier = 0;
    // for (auto &landmark : active_landmarks)
    // {
    //     if (landmark.second->isOutlier())
    //         continue;
    //     auto observations = landmark.second->observations();
    //     for (auto &obs : observations)
    //     {
    //         if (obs.lock() == nullptr)
    //             continue;
    //         auto feature = obs.lock();
    //         if (feature->isOutlier() || feature->getFrame() == nullptr)
    //             continue;
    //         auto frame = feature->getFrame();
    //         auto iter = active_kfs.find(frame->keyFrameId());
    //         if (iter == active_kfs.end())
    //             continue;
    //         auto keyframe = (*iter).second;

    //         Vec2 error =
    //             toVec2(feature->keyPoint()) - toVec2(camera_->world2pixel(landmark.second->pos(), keyframe->pose()));
    //         if (error.norm() > min_reprojerr_)
    //         {
    //            landmark.second->setOutlier(true);
    //             feature->setOutlier(true);                
    //             landmark.second->removeAllObservations();
    //             cnt_outlier++;
    //         }
    //         else
    //         {
    //             cnt_inlier++;
    //         }
    //     }
    // }
    // LOG(INFO) << "Outlier/Inlier in optimization: " << cnt_outlier << "/" << cnt_inlier;

    #endif
}