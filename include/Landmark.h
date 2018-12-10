/**
 * This file is part of CubeSLAM.
 *
 * Copyright (C) 2018, Carnegie Mellon University
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#ifndef LANDMARK_H
#define LANDMARK_H

#include "Cuboid.h"
#include "KeyFrame.h"
#include "MapPoint.h"

#include <g2o_Object.h>

namespace ORB_SLAM2 {

class MapPoint;
class KeyFrame;

class Landmark {
public:
    std::unordered_map<int, cv::Point2f> bboxCenter;
    g2o::VertexCuboid* cube_vertex;
    float meas_quality;
public:
    Landmark();
    Landmark(Landmark& other);
    Landmark(const Cuboid2D& proposal, const cv::Rect& bbox, KeyFrame* pKF, const cv::Mat& invK, int classIdx);

    void SetDimension(const Dimension3D& dimension);
    void SetPose(const cv::Mat& Tlw_);
    void SetPose(const cv::Mat& Rlw, const cv::Mat& tlw);
    Dimension3D GetDimension();
    void SetPoseAndDimension(const g2o::cuboid& Cuboid_);
    cv::Mat GetPose();
    cv::Mat GetPoseInverse();
    cv::Mat GetRotation();
    cv::Mat GetTranslation();

    cv::Mat GetCentroid();
    cv::Point2f GetProjectedCentroid(const cv::Mat& Tcw, const cv::Mat& K);

    g2o::cuboid GetCuboid();
    Cuboid2D Project(const cv::Mat& Tcw, const cv::Mat& K);
public:
    int mClassIdx;
    int landmarkID;
private:
    // SE3 Pose.
    cv::Mat Tlw;
    cv::Mat Twl;
    // Landmark centroid.
    cv::Mat Lw;
    // Landmark dimension.
    Dimension3D mDimension;
    g2o::cuboid mCuboid;  //cube_value

    std::mutex mMutexPose;

    void SetDimensionNoLock(const Dimension3D& dimension);
    void SetPoseNoLock(const cv::Mat& Tlw_);
    void SetPoseNoLock(const cv::Mat& Rlw, const cv::Mat& tlw);
};

inline cv::Mat TFromRt(const cv::Mat& R, const cv::Mat& t)
{
    cv::Mat T = cv::Mat::eye(4, 4, R.type());
    R.copyTo(T.rowRange(0, 3).colRange(0, 3));
    t.copyTo(T.col(3).rowRange(0, 3));
    return T;
}

template<class T>
inline float DistanceSquare(const cv::Point_<T>& pt1, const cv::Point_<T>& pt2)
{
    return powf(pt1.x - pt2.x, 2) + powf(pt1.y - pt2.y, 2);
}

template<class T>
inline float Distance(const cv::Point_<T>& pt1, const cv::Point_<T>& pt2)
{
    return sqrtf(DistanceSquare(pt1, pt2));
}

template<class T1, class T2>
inline bool Inside(const cv::Point_<T1>& pt, const cv::Rect_<T2>& bbox)
{
    return pt.x >= bbox.x && pt.x <= bbox.x + bbox.width && pt.y >= bbox.y && pt.y <= bbox.y + bbox.height;
}

}

#endif //LANDMARK_H
