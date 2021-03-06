/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "LoopClosingInterRobot.h"

#include "Sim3Solver.h"

#include "Converter.h"

#include "Optimizer.h"

#include "ORBmatcher.h"

#include<mutex>
#include<thread>


namespace ORB_SLAM2
{

LoopClosingInterRobot::LoopClosingInterRobot(Map *pMap, KeyFrameDatabase *pDB, ORBVocabulary *pVoc, const bool bFixScale, int robotID, char robotName):
    mbResetRequested(false), mbFinishRequested(false), mbFinished(true), mpMap(pMap),
    mpKeyFrameDB(pDB), mpORBVocabulary(pVoc), mpMatchedKF(NULL), mLastLoopKFid(0), mbRunningGBA(false), mbFinishedGBA(true),
    mbStopGBA(false), mpThreadGBA(NULL), mbFixScale(bFixScale), mnFullBAIdx(0), loopClosureRetreived_(true), loopClosure_(),
    robotID_(robotID), robotName_(robotName), keyframes_()
{
    mnCovisibilityConsistencyTh = 3;

    // Start keyframe publisher
    char robotString[100];
    sprintf(robotString, "%c", robotName);
    ros::NodeHandle n(robotString); // all messages will be published in this name space

    // Start the publisher
    keyframe_pub_ = n.advertise<distributed_mapper_msgs::Keyframe>("/keyframe", 1000); // Publish in global space
    measurement_pub_ = n.advertise<distributed_mapper_msgs::Measurement>("/measurement", 1000); // pubilsh relative pose

    // Start the subscriber
    keyframe_sub_ = n.subscribe("/keyframe", 1000, &LoopClosingInterRobot::Subscribe, this); // queue of 1000 keyframes
    cout << "Started loop closing between robots" << endl;
}

void LoopClosingInterRobot::SetTracker(Tracking *pTracker)
{
    mpTracker=pTracker;
}

void LoopClosingInterRobot::SetLocalMapper(LocalMapping *pLocalMapper)
{
    mpLocalMapper=pLocalMapper;
}

bool LoopClosingInterRobot::Match(distributed_mapper_msgs::Keyframe keyframe){
    // Received message
    // Convert wordIds and weights to BoW vector
    DBoW2::BowVector mBowVec;
    for(int wordId_i = 0; wordId_i < keyframe.wordIds.size(); wordId_i++){
        DBoW2::WordId wordId = keyframe.wordIds.at(wordId_i);
        DBoW2::WordValue wordValue = keyframe.wordValues.at(wordId_i);
        mBowVec.addWeight(wordId, wordValue);
    }

    // Min score
    float minScore = keyframe.minScore;

    // mnId
    int mnId = keyframe.symbolIndex;

    // Detect loop candidates (todo: check covisibility consistency)
    if(DetectLoop(mBowVec, mnId, minScore)){

        // extract keypoints and desc for further processing
        vector<cv::KeyPoint> keypoints;
        for(size_t keypoint_i = 0; keypoint_i < keyframe.keypoints.size(); keypoint_i++){
            distributed_mapper_msgs::Keypoint keypointMsg = keyframe.keypoints.at(keypoint_i);
            cv::KeyPoint keypoint;
            keypoint.pt.x = keypointMsg.x;
            keypoint.pt.y = keypointMsg.y;
            keypoint.angle = keypointMsg.angle;
            keypoint.octave = keypointMsg.octave ;
            keypoint.response = keypointMsg.response ;
            keypoint.size = keypointMsg.size;
            keypoint.class_id = keypointMsg.class_id;
            keypoints.push_back(keypoint); // Push to keypoints
        }

        float mfGridElementWidthInv = keyframe.mfGridElementWidthInv;
        float mfGridElementHeightInv = keyframe.mfGridElementHeightInv;
        float mnMinX = keyframe.mnMinX;
        float mnMinY = keyframe.mnMinY;
        float mnMaxX = keyframe.mnMaxX;
        float mnMaxY = keyframe.mnMaxY;

        // Assign features to grid
        std::vector<std::size_t> mGrid[FRAME_GRID_COLS][FRAME_GRID_ROWS];
        int N = keypoints.size();
        int nReserve = 0.5f*N/(FRAME_GRID_COLS*FRAME_GRID_ROWS);
        for(unsigned int i=0; i<FRAME_GRID_COLS;i++)
            for (unsigned int j=0; j<FRAME_GRID_ROWS;j++)
                mGrid[i][j].reserve(nReserve);

        for(int i=0;i<N;i++)
        {
            const cv::KeyPoint &kp = keypoints[i];
            int nGridPosX, nGridPosY;

            nGridPosX = round((kp.pt.x-mnMinX)*mfGridElementWidthInv);
            nGridPosY = round((kp.pt.y-mnMinY)*mfGridElementHeightInv);
            //Keypoint's coordinates are undistorted, which could cause to go out of the image
            if(nGridPosX<0 || nGridPosX>=FRAME_GRID_COLS || nGridPosY<0 || nGridPosY>=FRAME_GRID_ROWS)
                continue;
            mGrid[nGridPosX][nGridPosY].push_back(i);
        }

        std::vector< std::vector <std::vector<size_t> > > mGridVec;
        mGridVec.resize(FRAME_GRID_COLS);
        for(int i=0; i<FRAME_GRID_COLS;i++){
            mGridVec[i].resize(FRAME_GRID_ROWS);
            for(int j=0; j<FRAME_GRID_ROWS; j++)
                mGridVec[i][j] = mGrid[i][j];
        }

        // Create descriptor mat
        cv_bridge::CvImagePtr descriptorPtr = cv_bridge::toCvCopy(keyframe.desc, sensor_msgs::image_encodings::TYPE_8UC1);
        cv::Mat descriptors = descriptorPtr->image;

        // Create point descriptor mat
        cv_bridge::CvImagePtr pointDescriptorPtr = cv_bridge::toCvCopy(keyframe.pointDesc, sensor_msgs::image_encodings::TYPE_8UC1);
        cv::Mat pointDescriptors = pointDescriptorPtr->image;

        // Bag of words feature vector
        DBoW2::FeatureVector mFeatVec;
        for(int node_i = 0; node_i < keyframe.nodeIds.size(); node_i++){
            DBoW2::NodeId nodeID = keyframe.nodeIds.at(node_i);
            distributed_mapper_msgs::Indices indicesMsg = keyframe.indicesVec.at(node_i);
            for(int feat_i = 0; feat_i < indicesMsg.indices.size(); feat_i++){
                mFeatVec.addFeature(nodeID, indicesMsg.indices.at(feat_i));
            }
        }

        // nrMappoints
        int nrMapPoints = keyframe.nrMapPoints;

        // map point -> feature indices, maxDistInvariance, minDistInvariance, point descriptor vec
        vector<int> indices; vector<float> maxDistInvariance; vector<float> minDistInvariance; vector<cv::Mat> pointDescVec;
        for(int indices_i = 0; indices_i < keyframe.indices.size(); indices_i++){
            indices.push_back(keyframe.indices.at(indices_i));
            maxDistInvariance.push_back(keyframe.maxDistInvariance.at(indices_i));
            minDistInvariance.push_back(keyframe.minDistInvariance.at(indices_i));
            cv::Mat descriptor(1, 32, CV_8UC1);
            pointDescriptors.row(indices_i).copyTo(descriptor);
            pointDescVec.push_back(descriptor);
        }

        // Point locations
        vector<cv::Mat> worldPoints;
        for(int point_i = 0; point_i < nrMapPoints; point_i++){
            cv::Mat worldPoint(3, 1, CV_32F);
            for(int dim = 0; dim < 3; dim++){
                worldPoint.at<float>(dim, 0) = keyframe.worldPoints.at(point_i*3 + dim);
            }
            worldPoints.push_back(worldPoint);
        }

        // Write mvLevelSigma2
        vector<float> mvLevelSigma2;
        for(size_t i = 0; i < keyframe.mvLevelSigma2.size(); i++){
            mvLevelSigma2.push_back(keyframe.mvLevelSigma2.at(i));
        }

        vector<float> mvInvLevelSigma2;
        for(size_t i = 0; i < keyframe.mvInvLevelSigma2.size(); i++){
            mvInvLevelSigma2.push_back(keyframe.mvInvLevelSigma2.at(i));
        }


        // Write mvScaleFactors
        vector<float> mvScaleFactors;
        for(size_t i = 0; i < keyframe.mvScaleFactors.size(); i++){
            mvScaleFactors.push_back(keyframe.mvScaleFactors.at(i));
        }

        // Write  pose
        cv::Mat pose(3, 4, CV_32F);
        for(size_t row_i = 0; row_i < 3; row_i ++){
            for(size_t col_i = 0; col_i < 4; col_i ++){
                pose.at<float>(row_i, col_i) =keyframe.pose.at(row_i*4 + col_i);
            }
        }

        // Write calibration
        cv::Mat K(3, 3, CV_32F);
        for(size_t row_i = 0; row_i < 3; row_i ++){
            for(size_t col_i = 0; col_i < 3; col_i ++){
                K.at<float>(row_i, col_i) =keyframe.K.at(row_i*3 + col_i);
            }
        }

        float fx = keyframe.fx;
        float fy = keyframe.fy;
        float cx = keyframe.cx;
        float cy = keyframe.cy;
        float mfLogScaleFactor = keyframe.mfLogScaleFactor;
        int mnScaleLevels = keyframe.mnScaleLevels;

        //cout << "Extracted information: " << endl;

        // Compute similarity transformation [sR|t]
        // In the stereo/RGBD case s=1

        /*
          bool ComputeSim3(const vector<cv::Mat>& mapPoints,
                                                    const vector<cv::KeyPoint>& keypoints,
                                                    vector<int> indices,
                                                    const vector<float>& mvLevelSigma2,
                                                    const vector<float>& mvInvLevelSigma2,
                                                    cv::Mat pose, cv::Mat K,
                                                    const cv::Mat& descriptors,
                                                    const DBoW2::FeatureVector& mFeatVec,
                                                    int nrMapPoints, const vector<float> &maxDistanceInvariance,
                           const vector<float> &minDistanceInvariance, const vector<float> &mvScaleFactors,
                           const vector<cv::Mat> &pointDescriptors, float mnMinX, float mnMinY, float mnMaxX,
                           float mnMaxY, float mfGridElementWidthInv, float mfGridElementHeightInv, float mnGridRows,
                           float mnGridCols, int mnScaleLevels, float mvLogScaleFactor,
                           std::vector<std::vector<std::vector<size_t> > > mGrid,
                           float fx, float fy, float cx, float cy);

                           */
        if(ComputeSim3(worldPoints, keypoints, indices, mvLevelSigma2, mvInvLevelSigma2,
                       pose, K, descriptors, mFeatVec, nrMapPoints, maxDistInvariance,
                       minDistInvariance, mvScaleFactors,
                       pointDescVec,mnMinX, mnMinY, mnMaxX, mnMaxY, mfGridElementWidthInv, mfGridElementHeightInv,
                       FRAME_GRID_ROWS, FRAME_GRID_COLS, mnScaleLevels, mfLogScaleFactor, mGridVec,
                       fx, fy, cx, cy))
        {
            // Publish it
            distributed_mapper_msgs::Measurement measurementMsg; // key frame message
            measurementMsg.symbolChr1 = keyframe.symbolChr;
            measurementMsg.symbolIndex1 = keyframe.symbolIndex;
            measurementMsg.symbolChr2 = matchedSymbol_;
            measurementMsg.symbolIndex2 = matchedIndex_;
            for(int i =0; i < estimatedR_.rows*estimatedR_.cols; i++)
                measurementMsg.relativeRotation.push_back(estimatedR_.at<float>(i));
            for(int i =0; i < estimatedT_.rows*estimatedT_.cols; i++)
                measurementMsg.relativeTranslation.push_back(estimatedT_.at<float>(i));
            measurementMsg.relativeScale = estimatedS_;
            measurement_pub_.publish(measurementMsg);
            return true;
        }
        else{
            return false;
        }
    }
    else{
        return false;
    }
}


void LoopClosingInterRobot::Subscribe(const distributed_mapper_msgs::Keyframe& keyframe)
{
    // Only receive keyframe message from higher robotID
    if(keyframe.robotID > robotID_){

        // Add to keyframes vector
        int robotID = keyframe.robotID;
        if(!keyframes_.count(robotID)){
            std::vector<distributed_mapper_msgs::Keyframe> keyframeVec;
            keyframeVec.push_back(keyframe);
            keyframes_.insert(std::pair<int, std::vector<distributed_mapper_msgs::Keyframe> >(robotID, keyframeVec));
        }
        else{
            keyframes_[robotID].push_back(keyframe);
        }

        // Match it
        cout << endl << "[----LoopClosingInterRobot] Received message from: " << keyframe.robotID << " id: " << keyframe.symbolIndex << endl;
        Match(keyframe);
    }
}

// Match a window of keyframes
void LoopClosingInterRobot::MatchPreviousKeyFrames(){
    std::cout << "Matching previous keyframes: " << std::endl;

       std::map<int, std::vector<distributed_mapper_msgs::Keyframe> >::iterator it;

       // Iterate over robotIDs and match current keyframes
       for(it = keyframes_.begin(); it!=keyframes_.end(); it++){
           int robotID = it->first;
           std::vector<distributed_mapper_msgs::Keyframe> keyframes = it->second;
           mvConsistentGroups.clear();

           // Iterate over keyframes and match
           for(int keyframe_i = 0; keyframe_i < keyframes.size(); keyframe_i++){
               std::cout << " (" << robotID << "," << keyframe_i << ") " << std::endl;
               bool matched = Match(keyframes[keyframe_i]);
               if(matched)
                   break;
           }
       }

       // Clear the keyframes
       keyframes_.clear();
}

void LoopClosingInterRobot::Publish()
{
    mbFinished =false;

    while(1)
    {
        // Check if there are keyframes in the queue
        if(CheckNewKeyFrames())
        {
            // Publish new keyframe to nearby robots whoever is listening
            publishKeyFrame();
        }

        ResetIfRequested();

        if(CheckFinish())
            break;

        usleep(5000);
    }
    SetFinish();
}

void LoopClosingInterRobot::setLoopClosureRetrievedToTrue()
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    loopClosureRetreived_ = true;
}

void LoopClosingInterRobot::setLoopClosureRetrievedToFalse()
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    loopClosureRetreived_ = false;
}

bool LoopClosingInterRobot::LoopClosureIsRetrieved()
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    return loopClosureRetreived_;
}

void LoopClosingInterRobot::InsertKeyFrame(KeyFrame *pKF)
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    if(pKF->mnId!=0)
        mlpLoopKeyFrameQueue.push_back(pKF);
}

bool LoopClosingInterRobot::CheckNewKeyFrames()
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    return(!mlpLoopKeyFrameQueue.empty());
}

bool LoopClosingInterRobot::publishKeyFrame()
{
    {
        unique_lock<mutex> lock(mMutexLoopQueue);
        mpCurrentKF = mlpLoopKeyFrameQueue.front();
        mlpLoopKeyFrameQueue.pop_front();
        // Avoid that a keyframe can be erased while it is being process by this thread
        std::cout << "New keyframe added: " << std::endl;
        mpCurrentKF->SetNotErase();
    }

    distributed_mapper_msgs::Keyframe keyFrameMsg; // key frame message

    // Compute reference BoW similarity score
    // This is the lowest score to a connected keyframe in the covisibility graph
    // We will impose loop candidates to have a higher similarity than this
    const vector<KeyFrame*> vpConnectedKeyFrames = mpCurrentKF->GetVectorCovisibleKeyFrames();
    const DBoW2::BowVector &CurrentBowVec = mpCurrentKF->mBowVec;
    float minScore = 1;
    for(size_t i=0; i<vpConnectedKeyFrames.size(); i++)
    {
        KeyFrame* pKF = vpConnectedKeyFrames[i];
        if(pKF->isBad())
            continue;
        const DBoW2::BowVector &BowVec = pKF->mBowVec;

        float score = mpORBVocabulary->score(CurrentBowVec, BowVec);

        if(score<minScore)
            minScore = score;
    }
    keyFrameMsg.minScore=minScore;

    // Misc
    keyFrameMsg.mfLogScaleFactor= mpCurrentKF->mfLogScaleFactor;
    keyFrameMsg.mnScaleLevels = mpCurrentKF->mnScaleLevels;
    keyFrameMsg.mfGridElementWidthInv = mpCurrentKF->mfGridElementWidthInv;
    keyFrameMsg.mfGridElementHeightInv = mpCurrentKF->mfGridElementHeightInv;
    keyFrameMsg.mnMinX = mpCurrentKF->mnMinX;
    keyFrameMsg.mnMinY = mpCurrentKF->mnMinY;
    keyFrameMsg.mnMaxX = mpCurrentKF->mnMaxX;
    keyFrameMsg.mnMaxY = mpCurrentKF->mnMaxY;

    // Extract bag of words vector
    for(DBoW2::BowVector::const_iterator vit=mpCurrentKF->mBowVec.begin(), vend=mpCurrentKF->mBowVec.end(); vit != vend; vit++)
    {
        keyFrameMsg.wordIds.push_back(vit->first);
        keyFrameMsg.wordValues.push_back(vit->second);
    }

    // Set header
    gtsam::Key key = mpCurrentKF->key_;
    keyFrameMsg.symbolChr= gtsam::symbolChr(key);
    keyFrameMsg.symbolIndex= gtsam::symbolIndex(key);
    keyFrameMsg.robotID = robotID_;

    // Add keypoints
    const vector<cv::KeyPoint> &keypoints = mpCurrentKF->mvKeysUn;
    for(int keypoint_i = 0; keypoint_i < keypoints.size(); keypoint_i++){
        distributed_mapper_msgs::Keypoint keypointMsg;
        cv::KeyPoint keypoint = keypoints.at(keypoint_i);
        keypointMsg.x = keypoint.pt.x;
        keypointMsg.y = keypoint.pt.y;
        keypointMsg.angle = keypoint.angle;
        keypointMsg.octave = keypoint.octave;
        keypointMsg.response = keypoint.response;
        keypointMsg.size = keypoint.size;
        keypointMsg.class_id = keypoint.class_id;
        keyFrameMsg.keypoints.push_back(keypointMsg); // Push to keyframe
    }

    // Extract feature vector
    for(DBoW2::FeatureVector::const_iterator vit=mpCurrentKF->mFeatVec.begin(), vend=mpCurrentKF->mFeatVec.end(); vit != vend; vit++)
    {
        keyFrameMsg.nodeIds.push_back(vit->first);

        // Indices
        distributed_mapper_msgs::Indices indicesMsg;
        std::vector<unsigned int> features = vit->second;
        for(int feature_i = 0; feature_i < features.size(); feature_i++){
            indicesMsg.indices.push_back(features.at(feature_i));
        }
        keyFrameMsg.indicesVec.push_back(indicesMsg);
    }

    // Write descriptor
    const cv::Mat &Descriptors = mpCurrentKF->mDescriptors;
    cv_bridge::CvImage desc;
    desc.encoding = sensor_msgs::image_encodings::TYPE_8UC1;
    desc.image = Descriptors;
    keyFrameMsg.desc = *(desc.toImageMsg());

    // Write valid map points
    const vector<MapPoint*> vpMapPoints = mpCurrentKF->GetMapPointMatches();
    cv::Mat pointDescriptors = cv::Mat::zeros(vpMapPoints.size(), 32, CV_8UC1); // map point descriptors
    for(size_t i = 0; i < vpMapPoints.size(); i++){
        MapPoint* mapPoint = vpMapPoints[i];
        if(!mapPoint || mapPoint->isBad()){
            keyFrameMsg.indices.push_back(-1);
            for(size_t pos_i = 0; pos_i < 3; pos_i++){
                keyFrameMsg.worldPoints.push_back(-1);
            }
            keyFrameMsg.maxDistInvariance.push_back(-1);
            keyFrameMsg.minDistInvariance.push_back(-1);
            keyFrameMsg.mfMaxDistance.push_back(-1);
        }
        else{
            int index = mapPoint->GetIndexInKeyFrame(mpCurrentKF);
            keyFrameMsg.indices.push_back(index);

            cv::Mat worldPos = mapPoint->GetWorldPos();
            for(size_t pos_i = 0; pos_i < worldPos.rows*worldPos.cols; pos_i++){
                keyFrameMsg.worldPoints.push_back(worldPos.at<float>(pos_i));
            }

            float maxDistInvariance = mapPoint->GetMaxDistanceInvariance();
            float minDistInvariance = mapPoint->GetMinDistanceInvariance();
            keyFrameMsg.mfMaxDistance.push_back(mapPoint->mfMaxDistance);
            keyFrameMsg.maxDistInvariance.push_back(maxDistInvariance);
            keyFrameMsg.minDistInvariance.push_back(minDistInvariance);
            const cv::Mat dMP = mapPoint->GetDescriptor();
            dMP.copyTo(pointDescriptors.row(i));
        }
    }
    keyFrameMsg.nrMapPoints = vpMapPoints.size();

    cv_bridge::CvImage pointDesc;
    pointDesc.encoding = sensor_msgs::image_encodings::TYPE_8UC1;
    pointDesc.image = pointDescriptors;
    keyFrameMsg.pointDesc = *(pointDesc.toImageMsg());

    // Write mvLevelSigma2
    for(size_t i = 0; i < mpCurrentKF->mvLevelSigma2.size(); i++){
        keyFrameMsg.mvLevelSigma2.push_back(mpCurrentKF->mvLevelSigma2.at(i));
    }

    // Write mvLevelSigma2
    for(size_t i = 0; i < mpCurrentKF->mvInvLevelSigma2.size(); i++){
        keyFrameMsg.mvInvLevelSigma2.push_back(mpCurrentKF->mvInvLevelSigma2.at(i));
    }

    // Write mvScaleFactors
    for(size_t i = 0; i < mpCurrentKF->mvScaleFactors.size(); i++){
        keyFrameMsg.mvScaleFactors.push_back(mpCurrentKF->mvScaleFactors.at(i));
    }

    // Write  pose
    cv::Mat pose = mpCurrentKF->GetPose();
    for(size_t i = 0; i < pose.rows*pose.cols; i++){
        keyFrameMsg.pose.push_back(pose.at<float>(i));
    }

    // Write  calibration
    cv::Mat K = mpCurrentKF->mK;
    for(size_t i = 0; i < K.rows*K.cols; i++){
        keyFrameMsg.K.push_back(K.at<float>(i));
    }
    keyFrameMsg.fx = mpCurrentKF->fx;
    keyFrameMsg.fy = mpCurrentKF->fy;
    keyFrameMsg.cx = mpCurrentKF->cx;
    keyFrameMsg.cy = mpCurrentKF->cy;

    // Publish it
    keyframe_pub_.publish(keyFrameMsg);
}

bool LoopClosingInterRobot::DetectLoop(const DBoW2::BowVector& keyFrameBoWVec, int mnId,  float minScore)
{
    // Query the database imposing the minimum score
    vector<KeyFrame*> vpCandidateKFs = mpKeyFrameDB->DetectLoopCandidatesInterRobot(keyFrameBoWVec, mnId, minScore);
    //cout << "[LoopClosingInterRobot] Found " << vpCandidateKFs.size() << " candidate Keyframes: " << endl;
    // If there are no loop candidates, just add new keyframe and return false
    if(vpCandidateKFs.empty())
    {
        return false;
    }

    // For each loop candidate check consistency with previous loop candidates
    // Each candidate expands a covisibility group (keyframes connected to the loop candidate in the covisibility graph)
    // A group is consistent with a previous group if they share at least a keyframe
    // We must detect a consistent loop in several consecutive keyframes to accept it
    mvpEnoughConsistentCandidates.clear();

    vector<ConsistentGroup> vCurrentConsistentGroups;
    vector<bool> vbConsistentGroup(mvConsistentGroups.size(),false);
    for(size_t i=0, iend=vpCandidateKFs.size(); i<iend; i++)
    {
        KeyFrame* pCandidateKF = vpCandidateKFs[i];

        set<KeyFrame*> spCandidateGroup = pCandidateKF->GetConnectedKeyFrames();
        spCandidateGroup.insert(pCandidateKF);

        bool bEnoughConsistent = false;
        bool bConsistentForSomeGroup = false;
        for(size_t iG=0, iendG=mvConsistentGroups.size(); iG<iendG; iG++)
        {
            set<KeyFrame*> sPreviousGroup = mvConsistentGroups[iG].first;

            bool bConsistent = false;
            for(set<KeyFrame*>::iterator sit=spCandidateGroup.begin(), send=spCandidateGroup.end(); sit!=send;sit++)
            {
                if(sPreviousGroup.count(*sit))
                {
                    bConsistent=true;
                    bConsistentForSomeGroup=true;
                    break;
                }
            }

            if(bConsistent)
            {
                int nPreviousConsistency = mvConsistentGroups[iG].second;
                int nCurrentConsistency = nPreviousConsistency + 1;
                if(!vbConsistentGroup[iG])
                {
                    ConsistentGroup cg = make_pair(spCandidateGroup,nCurrentConsistency);
                    vCurrentConsistentGroups.push_back(cg);
                    vbConsistentGroup[iG]=true; //this avoid to include the same group more than once
                }
                if(nCurrentConsistency>=mnCovisibilityConsistencyTh && !bEnoughConsistent)
                {
                    mvpEnoughConsistentCandidates.push_back(pCandidateKF);
                    bEnoughConsistent=true; //this avoid to insert the same candidate more than once
                }
            }
        }

        // If the group is not consistent with any previous group insert with consistency counter set to zero
        if(!bConsistentForSomeGroup)
        {
            ConsistentGroup cg = make_pair(spCandidateGroup,0);
            vCurrentConsistentGroups.push_back(cg);
        }
    }

    // Update Covisibility Consistent Groups
    mvConsistentGroups = vCurrentConsistentGroups;

    // Add Current Keyframe to database
    //mpKeyFrameDB->add(mpCurrentKF);

    if(mvpEnoughConsistentCandidates.empty())
    {
        // mpCurrentKF->SetErase();
        return false;
    }
    else
    {
        return true;
    }

    return false;
}

bool LoopClosingInterRobot::ComputeSim3(const vector<cv::Mat>& mapPoints,
                                        const vector<cv::KeyPoint>& keypoints,
                                        vector<int> indices,
                                        const vector<float>& mvLevelSigma2,
                                        const vector<float>& mvInvLevelSigma2,
                                        const cv::Mat& pose, const cv::Mat& K,
                                        const cv::Mat& descriptors,
                                        const DBoW2::FeatureVector& mFeatVec,
                                        int nrMapPoints, const vector<float>& maxDistanceInvariance, const vector<float> &minDistanceInvariance,
                                        const vector<float> &mvScaleFactors, const vector<cv::Mat>& pointDescriptors,
                                        float mnMinX, float mnMinY, float mnMaxX, float mnMaxY, float mfGridElementWidthInv, float mfGridElementHeightInv,
                                        float mnGridRows, float mnGridCols, int mnScaleLevels, float mfLogScaleFactor,
                                        const std::vector< std::vector <std::vector<size_t> > >& mGrid,
                                        float fx, float fy, float cx, float cy){

    // For each consistent loop candidate we try to compute a Sim3
    const int nInitialCandidates = mvpEnoughConsistentCandidates.size();

    // We compute first ORB matches for each candidate
    // If enough matches are found, we setup a Sim3Solver
    ORBmatcher matcher(0.75,true);

    vector<Sim3Solver*> vpSim3Solvers;
    vpSim3Solvers.resize(nInitialCandidates);

    vector<vector<MapPoint*> > vvpMapPointMatches;
    vvpMapPointMatches.resize(nInitialCandidates);

    vector<bool> vbDiscarded;
    vbDiscarded.resize(nInitialCandidates);

    std::multimap<int,int> sortedMatches;
    int nCandidates=0; //candidates with enough matches

    // Sort according to #common keywords
    for(int i=0; i<nInitialCandidates; i++)
    {
        KeyFrame* pKF = mvpEnoughConsistentCandidates[i];

        // avoid that local mapping erase it while it is being processed in this thread
        pKF->SetNotErase();

        if(pKF->isBad())
        {
            vbDiscarded[i] = true;
            continue;
        }

        int nmatches = matcher.SearchByBoWInterRobot(keypoints, mFeatVec, nrMapPoints, indices, descriptors, pKF,vvpMapPointMatches[i]);
        //cout << "[LoopClosingInterRobot::computeSim3] #Matches by BoW: " << nmatches << " to candidate: " << gtsam::symbolChr(pKF->key_) << gtsam::symbolIndex(pKF->key_) << endl;

        sortedMatches.insert(pair<int, int>(nmatches, i));

        if(nmatches<20)
        {
            vbDiscarded[i] = true;
            continue;
        }
        else
        {
            Sim3Solver* pSolver = new Sim3Solver(mapPoints, keypoints, indices, mvLevelSigma2, pose, K, pKF,vvpMapPointMatches[i],mbFixScale);
            pSolver->SetRansacParameters(0.99,20,300);
            vpSim3Solvers[i] = pSolver;
        }
        nCandidates++;
    }


    bool bMatch = false;

    // Perform alternatively RANSAC iterations for each candidate starting from the candidate having maximum number of BoW matches
    // until one is succesful or all fail
    std::multimap<int,int>::iterator it;

    while(nCandidates>0 && !bMatch)
    {
        for (it=sortedMatches.begin(); it!=sortedMatches.end(); ++it){

            int i = it->second;
            if(vbDiscarded[i])
                continue;

            KeyFrame* pKF = mvpEnoughConsistentCandidates[i];

            // Perform 5 Ransac Iterations
            vector<bool> vbInliers;
            int nInliers;
            bool bNoMore;

            Sim3Solver* pSolver = vpSim3Solvers[i];
            cv::Mat Scm  = pSolver->iterate(5,bNoMore,vbInliers,nInliers);

            // If Ransac reachs max. iterations discard keyframe
            if(bNoMore)
            {
                vbDiscarded[i]=true;
                nCandidates--;
            }

            // If RANSAC returns a Sim3, perform a guided matching and optimize with all correspondences
            if(!Scm.empty())
            {
                vector<MapPoint*> vpMapPointMatches(vvpMapPointMatches[i].size(), static_cast<MapPoint*>(NULL));
                for(size_t j=0, jend=vbInliers.size(); j<jend; j++)
                {
                    if(vbInliers[j]){
                        vpMapPointMatches[j]=vvpMapPointMatches[i][j];
                    }
                }

                cv::Mat R = pSolver->GetEstimatedRotation();
                cv::Mat t = pSolver->GetEstimatedTranslation();
                const float s = pSolver->GetEstimatedScale();
                //cout << "[LoopClosingInterRobot] Found #Inliers: " << nInliers  << " with " << gtsam::symbolChr(pKF->key_) << gtsam::symbolIndex(pKF->key_) << endl;



                matcher.SearchBySim3InterRobot(nrMapPoints, mapPoints, keypoints, indices, maxDistanceInvariance, minDistanceInvariance, mvScaleFactors,
                                               pointDescriptors,  mnMinX,  mnMinY,  mnMaxX,  mnMaxY,  mfGridElementWidthInv,  mfGridElementHeightInv,
                                               mnGridRows,  mnGridCols,  mnScaleLevels,  mfLogScaleFactor,  mGrid,
                                               descriptors,   pose,  K, fx,  fy,  cx,  cy,  pKF,vpMapPointMatches,s,R,t,7.5);
                // matcher.SearchBySim3(mpCurrentKF,pKF,vpMapPointMatches,s,R,t,7.5);

                int goodPoints = 0;
                for(size_t indices_i = 0; indices_i < indices.size(); indices_i++){
                    if(indices[indices_i]){
                        goodPoints++;
                    }
                }
                //cout << "#Good Points: " << goodPoints << endl;


                g2o::Sim3 gScm(Converter::toMatrix3d(R),Converter::toVector3d(t),s);
                const int nInliers = Optimizer::OptimizeSim3InterRobot(mapPoints, keypoints, indices, mvInvLevelSigma2,
                                                                       pose, K, pKF, vpMapPointMatches, gScm, 10, mbFixScale);
                //const int nInliers = Optimizer::OptimizeSim3(mpCurrentKF, pKF, vpMapPointMatches, gScm, 10, mbFixScale);


                // If optimization is succesful stop ransacs and continue
                if(nInliers>=20)
                {
                    bMatch = true;
                    mpMatchedKF = pKF;
                    g2o::Sim3 gSmw(Converter::toMatrix3d(pKF->GetRotation()),Converter::toVector3d(pKF->GetTranslation()),1.0);
                    mg2oScw = gScm*gSmw;
                    mScw = Converter::toCvMat(mg2oScw);
                    mScm = Converter::toCvMat(gScm);

                    estimatedR_ = mScm.rowRange(0,3).colRange(0,3).clone();;
                    estimatedT_ = mScm.rowRange(0,3).col(3).clone();
                    estimatedS_ = 1.0f;
                    matchedSymbol_ = gtsam::symbolChr(pKF->key_);
                    matchedIndex_ = gtsam::symbolIndex(pKF->key_);
                    mvpCurrentMatchedPoints = vpMapPointMatches;
                    break;
                }
            }

        }
    }


    if(!bMatch)
    {
        //        for(int i=0; i<nInitialCandidates; i++)
        //          mvpEnoughConsistentCandidates[i]->SetErase();
        //        mpCurrentKF->SetErase();
        return false;
    }

    // Retrieve MapPoints seen in Loop Keyframe and neighbors
    vector<KeyFrame*> vpLoopConnectedKFs = mpMatchedKF->GetVectorCovisibleKeyFrames();
    vpLoopConnectedKFs.push_back(mpMatchedKF);
    mvpLoopMapPoints.clear();
    for(vector<KeyFrame*>::iterator vit=vpLoopConnectedKFs.begin(); vit!=vpLoopConnectedKFs.end(); vit++)
    {
        KeyFrame* pKF = *vit;
        vector<MapPoint*> vpMapPoints = pKF->GetMapPointMatches();
        for(size_t i=0, iend=vpMapPoints.size(); i<iend; i++)
        {
            MapPoint* pMP = vpMapPoints[i];
            if(pMP)
            {
                if(!pMP->isBad() && pMP->mnLoopPointForKFInterRobot!=mpCurrentKF->mnId)
                {
                    mvpLoopMapPoints.push_back(pMP);
                    pMP->mnLoopPointForKFInterRobot=mpCurrentKF->mnId;
                }
            }
        }
    }


    // Find more matches projecting with the computed Sim3
    matcher.SearchByProjectionInterRobot(keypoints, mvScaleFactors,
                                         mnMinX,  mnMinY,  mnMaxX,  mnMaxY,  mfGridElementWidthInv,  mfGridElementHeightInv,
                                         mnGridRows,  mnGridCols,  mnScaleLevels, mfLogScaleFactor, mGrid,
                                         descriptors,  fx,  fy,  cx,  cy, mScw, mvpLoopMapPoints, mvpCurrentMatchedPoints,10);


    // If enough matches accept Loop
    int nTotalMatches = 0;
    for(size_t i=0; i<mvpCurrentMatchedPoints.size(); i++)
    {
        if(mvpCurrentMatchedPoints[i])
            nTotalMatches++;
    }

    if(nTotalMatches>=40)
    {
        //        for(int i=0; i<nInitialCandidates; i++)
        //          if(mvpEnoughConsistentCandidates[i]!=mpMatchedKF)
        //            mvpEnoughConsistentCandidates[i]->SetErase();
        return true;
    }
    else
    {
        //        for(int i=0; i<nInitialCandidates; i++)
        //          mvpEnoughConsistentCandidates[i]->SetErase();
        //        mpCurrentKF->SetErase();
        return false;
    }
    return true;

}

void LoopClosingInterRobot::CorrectLoop()
{
    cout << "Loop detected!" << endl;

    // Send a stop signal to Local Mapping
    // Avoid new keyframes are inserted while correcting the loop
    mpLocalMapper->RequestStop();

    // If a Global Bundle Adjustment is running, abort it
    if(isRunningGBA())
    {
        unique_lock<mutex> lock(mMutexGBA);
        mbStopGBA = true;

        mnFullBAIdx++;

        if(mpThreadGBA)
        {
            mpThreadGBA->detach();
            delete mpThreadGBA;
        }
    }

    // Wait until Local Mapping has effectively stopped
    while(!mpLocalMapper->isStopped())
    {
        usleep(1000);
    }

    // Ensure current keyframe is updated
    mpCurrentKF->UpdateConnections();

    // Retrive keyframes connected to the current keyframe and compute corrected Sim3 pose by propagation
    mvpCurrentConnectedKFs = mpCurrentKF->GetVectorCovisibleKeyFrames();
    mvpCurrentConnectedKFs.push_back(mpCurrentKF);

    KeyFrameAndPose CorrectedSim3, NonCorrectedSim3;
    CorrectedSim3[mpCurrentKF]=mg2oScw;
    cv::Mat Twc = mpCurrentKF->GetPoseInverse();


    {
        // Get Map Mutex
        unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

        for(vector<KeyFrame*>::iterator vit=mvpCurrentConnectedKFs.begin(), vend=mvpCurrentConnectedKFs.end(); vit!=vend; vit++)
        {
            KeyFrame* pKFi = *vit;

            cv::Mat Tiw = pKFi->GetPose();

            if(pKFi!=mpCurrentKF)
            {
                cv::Mat Tic = Tiw*Twc;
                cv::Mat Ric = Tic.rowRange(0,3).colRange(0,3);
                cv::Mat tic = Tic.rowRange(0,3).col(3);
                g2o::Sim3 g2oSic(Converter::toMatrix3d(Ric),Converter::toVector3d(tic),1.0);
                g2o::Sim3 g2oCorrectedSiw = g2oSic*mg2oScw;
                //Pose corrected with the Sim3 of the loop closure
                CorrectedSim3[pKFi]=g2oCorrectedSiw;
            }

            cv::Mat Riw = Tiw.rowRange(0,3).colRange(0,3);
            cv::Mat tiw = Tiw.rowRange(0,3).col(3);
            g2o::Sim3 g2oSiw(Converter::toMatrix3d(Riw),Converter::toVector3d(tiw),1.0);
            //Pose without correction
            NonCorrectedSim3[pKFi]=g2oSiw;
        }

        // Correct all MapPoints obsrved by current keyframe and neighbors, so that they align with the other side of the loop
        for(KeyFrameAndPose::iterator mit=CorrectedSim3.begin(), mend=CorrectedSim3.end(); mit!=mend; mit++)
        {
            KeyFrame* pKFi = mit->first;
            g2o::Sim3 g2oCorrectedSiw = mit->second;
            g2o::Sim3 g2oCorrectedSwi = g2oCorrectedSiw.inverse();

            g2o::Sim3 g2oSiw =NonCorrectedSim3[pKFi];

            vector<MapPoint*> vpMPsi = pKFi->GetMapPointMatches();
            for(size_t iMP=0, endMPi = vpMPsi.size(); iMP<endMPi; iMP++)
            {
                MapPoint* pMPi = vpMPsi[iMP];
                if(!pMPi)
                    continue;
                if(pMPi->isBad())
                    continue;
                if(pMPi->mnCorrectedByKF==mpCurrentKF->mnId)
                    continue;

                // Project with non-corrected pose and project back with corrected pose
                cv::Mat P3Dw = pMPi->GetWorldPos();
                Eigen::Matrix<double,3,1> eigP3Dw = Converter::toVector3d(P3Dw);
                Eigen::Matrix<double,3,1> eigCorrectedP3Dw = g2oCorrectedSwi.map(g2oSiw.map(eigP3Dw));

                cv::Mat cvCorrectedP3Dw = Converter::toCvMat(eigCorrectedP3Dw);
                pMPi->SetWorldPos(cvCorrectedP3Dw);
                pMPi->mnCorrectedByKF = mpCurrentKF->mnId;
                pMPi->mnCorrectedReference = pKFi->mnId;
                pMPi->UpdateNormalAndDepth();
            }

            // Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
            Eigen::Matrix3d eigR = g2oCorrectedSiw.rotation().toRotationMatrix();
            Eigen::Vector3d eigt = g2oCorrectedSiw.translation();
            double s = g2oCorrectedSiw.scale();

            eigt *=(1./s); //[R t/s;0 1]

            cv::Mat correctedTiw = Converter::toCvSE3(eigR,eigt);

            pKFi->SetPose(correctedTiw);

            // Make sure connections are updated
            pKFi->UpdateConnections();
        }

        // Start Loop Fusion
        // Update matched map points and replace if duplicated
        for(size_t i=0; i<mvpCurrentMatchedPoints.size(); i++)
        {
            if(mvpCurrentMatchedPoints[i])
            {
                MapPoint* pLoopMP = mvpCurrentMatchedPoints[i];
                MapPoint* pCurMP = mpCurrentKF->GetMapPoint(i);
                if(pCurMP)
                    pCurMP->Replace(pLoopMP);
                else
                {
                    mpCurrentKF->AddMapPoint(pLoopMP,i);
                    pLoopMP->AddObservation(mpCurrentKF,i);
                    pLoopMP->ComputeDistinctiveDescriptors();
                }
            }
        }

    }

    // Project MapPoints observed in the neighborhood of the loop keyframe
    // into the current keyframe and neighbors using corrected poses.
    // Fuse duplications.
    SearchAndFuse(CorrectedSim3);


    // After the MapPoint fusion, new links in the covisibility graph will appear attaching both sides of the loop
    map<KeyFrame*, set<KeyFrame*> > LoopConnections;

    for(vector<KeyFrame*>::iterator vit=mvpCurrentConnectedKFs.begin(), vend=mvpCurrentConnectedKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;
        vector<KeyFrame*> vpPreviousNeighbors = pKFi->GetVectorCovisibleKeyFrames();

        // Update connections. Detect new links.
        pKFi->UpdateConnections();
        LoopConnections[pKFi]=pKFi->GetConnectedKeyFrames();
        for(vector<KeyFrame*>::iterator vit_prev=vpPreviousNeighbors.begin(), vend_prev=vpPreviousNeighbors.end(); vit_prev!=vend_prev; vit_prev++)
        {
            LoopConnections[pKFi].erase(*vit_prev);
        }
        for(vector<KeyFrame*>::iterator vit2=mvpCurrentConnectedKFs.begin(), vend2=mvpCurrentConnectedKFs.end(); vit2!=vend2; vit2++)
        {
            LoopConnections[pKFi].erase(*vit2);
        }
    }

    // Optimize graph
    Optimizer::OptimizeEssentialGraph(mpMap, mpMatchedKF, mpCurrentKF, NonCorrectedSim3, CorrectedSim3, LoopConnections, mbFixScale);

    // Add loop edge
    mpMatchedKF->AddLoopEdge(mpCurrentKF);
    mpCurrentKF->AddLoopEdge(mpMatchedKF);

    // Launch a new thread to perform Global Bundle Adjustment
    mbRunningGBA = true;
    mbFinishedGBA = false;
    mbStopGBA = false;
    mpThreadGBA = new thread(&LoopClosingInterRobot::RunGlobalBundleAdjustment,this,mpCurrentKF->mnId);

    // Loop closed. Release Local Mapping.
    mpLocalMapper->Release();

    //cout << "Loop Closed!" << endl;

    mLastLoopKFid = mpCurrentKF->mnId;
}

void LoopClosingInterRobot::SearchAndFuse(const KeyFrameAndPose &CorrectedPosesMap)
{
    ORBmatcher matcher(0.8);

    for(KeyFrameAndPose::const_iterator mit=CorrectedPosesMap.begin(), mend=CorrectedPosesMap.end(); mit!=mend;mit++)
    {
        KeyFrame* pKF = mit->first;

        g2o::Sim3 g2oScw = mit->second;
        cv::Mat cvScw = Converter::toCvMat(g2oScw);

        vector<MapPoint*> vpReplacePoints(mvpLoopMapPoints.size(),static_cast<MapPoint*>(NULL));
        matcher.Fuse(pKF,cvScw,mvpLoopMapPoints,4,vpReplacePoints);

        // Get Map Mutex
        unique_lock<mutex> lock(mpMap->mMutexMapUpdate);
        const int nLP = mvpLoopMapPoints.size();
        for(int i=0; i<nLP;i++)
        {
            MapPoint* pRep = vpReplacePoints[i];
            if(pRep)
            {
                pRep->Replace(mvpLoopMapPoints[i]);
            }
        }
    }
}


void LoopClosingInterRobot::RequestReset()
{
    {
        unique_lock<mutex> lock(mMutexReset);
        mbResetRequested = true;
    }

    while(1)
    {
        {
            unique_lock<mutex> lock2(mMutexReset);
            if(!mbResetRequested)
                break;
        }
        usleep(5000);
    }
}



void LoopClosingInterRobot::ResetIfRequested()
{
    unique_lock<mutex> lock(mMutexReset);
    if(mbResetRequested)
    {
        mvConsistentGroups.clear();
        mlpLoopKeyFrameQueue.clear();
        mLastLoopKFid=0;
        mbResetRequested=false;
    }
}

void LoopClosingInterRobot::RunGlobalBundleAdjustment(unsigned long nLoopKF)
{
    cout << "Starting Global Bundle Adjustment" << endl;

    int idx =  mnFullBAIdx;
    Optimizer::GlobalBundleAdjustemnt(mpMap,10,&mbStopGBA,nLoopKF,false);

    // Update all MapPoints and KeyFrames
    // Local Mapping was active during BA, that means that there might be new keyframes
    // not included in the Global BA and they are not consistent with the updated map.
    // We need to propagate the correction through the spanning tree
    {
        unique_lock<mutex> lock(mMutexGBA);
        if(idx!=mnFullBAIdx)
            return;

        if(!mbStopGBA)
        {
            cout << "Global Bundle Adjustment finished" << endl;
            cout << "Updating map ..." << endl;
            mpLocalMapper->RequestStop();
            // Wait until Local Mapping has effectively stopped

            while(!mpLocalMapper->isStopped() && !mpLocalMapper->isFinished())
            {
                usleep(1000);
            }

            // Get Map Mutex
            unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

            // Correct keyframes starting at map first keyframe
            list<KeyFrame*> lpKFtoCheck(mpMap->mvpKeyFrameOrigins.begin(),mpMap->mvpKeyFrameOrigins.end());

            while(!lpKFtoCheck.empty())
            {
                KeyFrame* pKF = lpKFtoCheck.front();
                const set<KeyFrame*> sChilds = pKF->GetChilds();
                cv::Mat Twc = pKF->GetPoseInverse();
                for(set<KeyFrame*>::const_iterator sit=sChilds.begin();sit!=sChilds.end();sit++)
                {
                    KeyFrame* pChild = *sit;
                    if(pChild->mnBAGlobalForKF!=nLoopKF)
                    {
                        cv::Mat Tchildc = pChild->GetPose()*Twc;
                        pChild->mTcwGBA = Tchildc*pKF->mTcwGBA;//*Tcorc*pKF->mTcwGBA;
                        pChild->mnBAGlobalForKF=nLoopKF;

                    }
                    lpKFtoCheck.push_back(pChild);
                }

                pKF->mTcwBefGBA = pKF->GetPose();
                pKF->SetPose(pKF->mTcwGBA);
                lpKFtoCheck.pop_front();
            }

            // Correct MapPoints
            const vector<MapPoint*> vpMPs = mpMap->GetAllMapPoints();

            for(size_t i=0; i<vpMPs.size(); i++)
            {
                MapPoint* pMP = vpMPs[i];

                if(pMP->isBad())
                    continue;

                if(pMP->mnBAGlobalForKF==nLoopKF)
                {
                    // If optimized by Global BA, just update
                    pMP->SetWorldPos(pMP->mPosGBA);
                }
                else
                {
                    // Update according to the correction of its reference keyframe
                    KeyFrame* pRefKF = pMP->GetReferenceKeyFrame();

                    if(pRefKF->mnBAGlobalForKF!=nLoopKF)
                        continue;

                    // Map to non-corrected camera
                    cv::Mat Rcw = pRefKF->mTcwBefGBA.rowRange(0,3).colRange(0,3);
                    cv::Mat tcw = pRefKF->mTcwBefGBA.rowRange(0,3).col(3);
                    cv::Mat Xc = Rcw*pMP->GetWorldPos()+tcw;

                    // Backproject using corrected camera
                    cv::Mat Twc = pRefKF->GetPoseInverse();
                    cv::Mat Rwc = Twc.rowRange(0,3).colRange(0,3);
                    cv::Mat twc = Twc.rowRange(0,3).col(3);

                    pMP->SetWorldPos(Rwc*Xc+twc);
                }
            }

            mpLocalMapper->Release();

            cout << "Map updated!" << endl;
        }

        mbFinishedGBA = true;
        mbRunningGBA = false;
    }
}

void LoopClosingInterRobot::RequestFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinishRequested = true;
}

bool LoopClosingInterRobot::CheckFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinishRequested;
}

void LoopClosingInterRobot::SetFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinished = true;
}

bool LoopClosingInterRobot::isFinished()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinished;
}


} //namespace ORB_SLAM
