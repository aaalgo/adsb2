#include <sstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <boost/format.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <glog/logging.h>
#include "adsb2.h"

namespace fs = boost::filesystem;

using namespace std;
using namespace cv;
using namespace adsb2;

int main(int argc, char **argv) {
    //Stack stack("sax", "tmp");
    namespace po = boost::program_options; 
    string config_path;
    vector<string> overrides;
    string list_path;
    string root_dir;

    po::options_description desc("Allowed options");
    desc.add_options()
    ("help,h", "produce help message.")
    ("config", po::value(&config_path)->default_value("adsb2.xml"), "config file")
    ("override,D", po::value(&overrides), "override configuration.")
    ("list", po::value(&list_path), "")
    ("root", po::value(&root_dir), "")
    ;


    po::positional_options_description p;
    p.add("list", 1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
                     options(desc).positional(p).run(), vm);
    po::notify(vm); 

    if (vm.count("help") || list_path.empty()) {
        cerr << desc;
        return 1;
    }

    Config config;
    try {
        LoadConfig(config_path, &config);
    } catch (...) {
        cerr << "Failed to load config file: " << config_path << ", using defaults." << endl;
    }
    OverrideConfig(overrides, &config);

    Cook cook(config);
    Samples samples(list_path, root_dir, cook);
    Detector *det = make_caffe_detector(config.get<string>("adsb2.caffe.model", "model"));
    CHECK(det) << " cannot create detector.";

    float th = config.get<float>("adsb2.bound_th", 0.95);
    for (auto &s: samples) {
        Mat prob;
        det->apply(s, &prob);
        Rect bb;
        bound(prob, &bb, th);
        float s1, s2;
        s.eval(prob, &s1, &s2);
        cout << s1 << '\t' << bb.width * bb.height * s.meta.spacing << endl;
        /*
        cv::rectangle(image, bb, cv::Scalar(0xFF));
        */
    }

    delete det;
}
