#include <iostream>
#include <sstream>

#include "gm2dl_io.h"
#include "edge_calib.h"
#include "g2o_optimize.h"
#include "yaml_wrapper.h"
#include "libviso2_wrapper.h"
#include "motion_information.h"
#include "closed_form_calibration.h"

// If you want check your graph's vertices and edges, please uncomment this.
//#define CHECK_GRAPH
#define CLOSED_FORM

using namespace std;

int main(int argc, char** argv)
{
    string rawFilename;
    bool use_viso = false;
    bool use_cfs = false;
    double init_x, init_y;

    cout << endl
            << "\033[32mA demo implementing the method of stereo-odo calibration.\033[0m"
            << endl << "* * * Author: \033[32mDoom @ ZJU.\033[0m"
            << endl << "Usage: sclam_odo_stereo_cal USE_VISO INPUT_FILENAME USE_CLOSEFORM" << endl << endl;

    if(argc < 2){
        cout << "\033[33m[Warning]:No input directory name, using the default one : /home/doom/CSO/data/params.yaml\033[0m" << endl << endl;
        // Read in our param file
        CYaml::get().parseParamFile("/home/doom/CSO/data/params.yaml");
        // Read in params
        use_viso = CYaml::get().general()["use_viso"].as<bool>();
        rawFilename = CYaml::get().general()["data_folder"].as<std::string>();
        use_cfs = CYaml::get().general()["use_closed_form"].as<bool>();
        init_x = CYaml::get().general()["init_x"].as<double>();
        init_y = CYaml::get().general()["init_y"].as<double>();
    }
    else if(argc == 2){
        cout << "\033[31mInput params file directory: \033[0m" << argv[1] << endl << endl;
        // Read in our param file
        CYaml::get().parseParamFile(argv[1]);
        // Read in params
        use_viso = CYaml::get().general()["use_viso"].as<bool>();
        rawFilename = CYaml::get().general()["data_folder"].as<std::string>();
        use_cfs = CYaml::get().general()["use_closed_form"].as<bool>();
        init_x = CYaml::get().general()["init_x"].as<double>();
        init_y = CYaml::get().general()["init_y"].as<double>();
    }
    else
    {
        cerr << "\033[31m[FATAL]Bad parameters.\033[0m" << endl;
        return 1;
    }

    // construct a new optimizer
    SparseOptimizer optimizer;
    initOptimizer(optimizer);

    // read the times.txt, to determine how many pics in this directory.
    stringstream ss;
    ss << rawFilename << "/votimes.txt";
    ifstream in(ss.str().c_str());
    int Num = 0;
    vector<double> timeque;
    if(in.fail())
    {
        cerr << "\033[1;31m[ERROR]Wrong foldername or no times.txt in this directory.\033[0m" << endl;
        return 1;
    }
    else
    {
        string stemp;
        getline(in, stemp, '\n');
        while(in.good()){
            Num++;
            getline(in, stemp, '\n');
        }
        cout << "There are " << Num << " pics in this direcotory." << endl << endl;
        in.close();
        in.open(ss.str().c_str());
        double temp;
        for(int i = 0; i < Num; i++)
        {
            in >> temp;
            timeque.push_back(temp);
        }
        in.close();
    }

    // loading odom data
    cerr << "\033[31mNow loading odometry data.\033[0m" << endl;
    string odomName = rawFilename + "/newodom.txt";
    DataQueue odometryQueue;
    DataQueue stereovoQueue;
    SE2 init_offset;
    int numOdom = Gm2dlIO::readRobotOdom(odomName, odometryQueue);
    if (numOdom == 0) {
      cerr << "No raw information read" << endl;
      return 0;
    }
    cerr << "Done...Read " << numOdom << " odom readings from file" << endl << endl;

    // initial first stereo frame pose
    SE2 initialStereoPose;
    bool first = true;
    int numVo = 0;
    if(use_viso)
    {
        cout << "\033[31mUsing libviso...\033[0m" << rawFilename << endl;
        RobotOdom* ro = dynamic_cast<RobotOdom*>(odometryQueue.buffer().begin()->second);
        // init a libviso2
        StereoVo viso(rawFilename, Num, ro, timeque);
        // get initial odometry pose in robot frame and stereo vo.
        viso.getInitStereoPose(initialStereoPose);
        numVo = viso.getMotion(stereovoQueue);
        cerr << "Done...There are " << numVo << " vo datas in the queue." << endl << endl;
    }
    else
    {
        cerr << "\033[31mLoading pose file...\033[0m" << endl;
        ss.str("");
        ss << rawFilename << "/CameraTrajectory.txt";
        in.open(ss.str().c_str());

        int numVo = 0;
        Matrix4D posemat;
        for(int i = 0; i < Num; i++)
        {
            for(int j = 0; j < 4; j++)
            {
                for(int k = 0; k < 4; k++)
                    in >> posemat(j, k);
            }
            if(first){
                init_offset.setTranslation(Eigen::Vector2d(init_x, init_y));
                init_offset.setRotation(Eigen::Rotation2Dd::Identity());
//                SE2 x(posemat(2,3),-posemat(0,3),acos(posemat(0,0)));
//                RobotOdom* ro = dynamic_cast<RobotOdom*>(odometryQueue.buffer().begin()->second);
                initialStereoPose = init_offset;
                first = false;
            }
            // save stereo vo as robotOdom node.
            RobotOdom* tempVo = new RobotOdom;
            tempVo->setTimestamp(timeque[i]);
            SE2 x(posemat(2,3),-posemat(0,3),acos(posemat(0,0)));
            tempVo->setOdomPose(init_offset*x);
            stereovoQueue.add(tempVo);
            numVo++;
        }
        in.close();
        cerr << "Done...There are " << numVo << " vo datas in the queue." << endl << endl;
    }

    // adding the measurements
    vector<MotionInformation, Eigen::aligned_allocator<MotionInformation> > motions;
    {
      std::map<double, RobotData*>::const_iterator it = stereovoQueue.buffer().begin();
      std::map<double, RobotData*>::const_iterator prevIt = it++;
      for (; it != stereovoQueue.buffer().end(); ++it) {
        MotionInformation mi;
        RobotOdom* prevVo = dynamic_cast<RobotOdom*>(prevIt->second);
        RobotOdom* curVo = dynamic_cast<RobotOdom*>(it->second);
        mi.stereoMotion = prevVo->odomPose().inverse() * curVo->odomPose();
        // get the motion of the robot in that time interval
        RobotOdom* prevOdom = dynamic_cast<RobotOdom*>(odometryQueue.findClosestData(prevVo->timestamp()));
        RobotOdom* curOdom = dynamic_cast<RobotOdom*>(odometryQueue.findClosestData(curVo->timestamp()));
        mi.odomMotion = prevOdom->odomPose().inverse() * curOdom->odomPose();
        mi.timeInterval = prevOdom->timestamp() - curOdom->timestamp();
        prevIt = it;
        motions.push_back(mi);
      }
    }

    // Construct the graph.
    cerr << "\033[31mConstruct the graph...\033[0m" << endl;
    // Vertex offset
    addVertexSE2(optimizer, initialStereoPose, 0);
    for(int i = 0; i < motions.size(); i++)
    {
        const SE2& odomMotion = motions[i].odomMotion;
        const SE2& stereoMotion = motions[i].stereoMotion;
        // add the edge
        // stereo vo and odom measurement.
        OdomAndStereoMotion  meas;
        meas.StereoMotion = stereoMotion;
        meas.odomMotion = odomMotion;
        addEdgeCalib(optimizer, 0, meas, Eigen::Matrix3d::Identity());
    }
    cerr << "Done..." << endl << endl;

    // if you want check the component of your graph, uncomment the CHECK_GRAPH
#ifdef CHECK_GRAPH
    for(auto it:optimizer.edges())
    {
        VertexSE2* v = static_cast<VertexSE2*>(it->vertex(0));
        cout << v->id() << endl;
    }
#endif

    // optimize.
    optimize(optimizer, 10);
    Eigen::Vector3d result = getEstimation(optimizer);
    cerr << "\033[1;32mCalibrated stereo offset (x, y, theta):\033[0m" << result[0] << " " << result [1] << " " << result[2] << endl << endl;

    // If you want see how's its closed form solution, uncomment the CLOSED_FORM
    if(use_cfs){
        cerr << "\033[31mPerforming the closed form solution...\033[0m" << endl;
        SE2 closedFormStereo;
        ClosedFormCalibration::calibrate(motions, closedFormStereo);
        cerr << "\033[1;32mDone... closed form Calibrated stereo offset (x, y, theta):\033[0m" << closedFormStereo.toVector().transpose() << endl;
    }

    return 0;
}
