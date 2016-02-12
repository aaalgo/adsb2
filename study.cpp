#include <sstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/program_options.hpp>
#include <boost/assert.hpp>
#include <glog/logging.h>
#include "adsb2.h"

using namespace std;
using namespace boost;
using namespace cv;
using namespace adsb2;

int main(int argc, char **argv) {
    nice(10);
    //Series stack("sax", "tmp");
    namespace po = boost::program_options; 
    string config_path;
    vector<string> overrides;
    string input_dir;
    string output_dir;
    int ca;
    int ca_it;
    int decap;
    /*
    string output_dir;
    string gif;
    float th;
    int mk;
    */

    po::options_description desc("Allowed options");
    desc.add_options()
    ("help,h", "produce help message.")
    ("config", po::value(&config_path)->default_value("adsb2.xml"), "config file")
    ("override,D", po::value(&overrides), "override configuration.")
    ("input,i", po::value(&input_dir), "")
    ("output,o", po::value(&output_dir), "")
    ("ca", po::value(&ca)->default_value(1), "")
    ("bound", "")
    ("no-gif", "")
    ("ca-it", po::value(&ca_it)->default_value(2), "")
    ("decap", po::value(&decap)->default_value(0), "")
    //("output,o", po::value(&output_dir), "")
    /*
    ("gif", po::value(&gif), "")
    ("th", po::value(&th)->default_value(0.90), "")
    ("mk", po::value(&mk)->default_value(5), "")
    */
    ;


    po::positional_options_description p;
    p.add("input", 1);
    p.add("output", 1);
    //p.add("output", 1);
    //p.add("output", 1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
                     options(desc).positional(p).run(), vm);
    po::notify(vm); 

    if (vm.count("help") || input_dir.empty()) {
        cerr << desc;
        return 1;
    }

    bool do_gif = vm.count("no-gif") == 0;

    Config config;
    try {
        LoadConfig(config_path, &config);
    } catch (...) {
        cerr << "Failed to load config file: " << config_path << ", using defaults." << endl;
    }
    OverrideConfig(overrides, &config);

    GlobalInit(argv[0], config);
    Cook cook(config);

    timer::auto_cpu_timer timer(cerr);
    Study study(input_dir, true, true, true);
    cook.apply(&study);
    cv::Rect bound;
    /*
    string bound_model = config.get("adsb2.caffe.bound_model", (home_dir/fs::path("bound_model")).native());
    if (vm.count("bound")) {
        Detector *bb_det = make_caffe_detector(bound_model);
        Bound(bb_det, &study, &bound, config);
        delete bb_det;
    }
    */

    ComputeBoundProb(&study);
    cerr << "Filtering..." << endl;
    ProbFilter(&study, config);
    vector<Slice *> slices;
    study.pool(&slices);
    cerr << "Finding squares..." << endl;
#pragma omp parallel for schedule(dynamic, 1)
    for (unsigned i = 0; i < slices.size(); ++i) {
        FindBox(slices[i], config);
    }
    ComputeContourProb(&study, config);
    study_CA1(&study, config, true);

    if (decap > 0) {
        CHECK(decap < 5);
        for (int i = 0; i < decap; ++i) {
            for (auto &s: study[i]) {
                s.area = 0;
            }
        }
    }
    else if (decap < 0) {
        RefineTop(&study, config);
    }
    
    Volume min, max;
    FindMinMaxVol(study, &min, &max, config);
    if (output_dir.size()) {
        cerr << "Saving output..." << endl;
        fs::path dir(output_dir);
        fs::create_directories(dir);
        {
            fs::ofstream vol(dir/fs::path("volume.txt"));
            vol << min.mean << '\t' << std::sqrt(min.var)
                << '\t' << max.mean << '\t' << std::sqrt(max.var) << endl;
        }
        {
            fs::ofstream vol(dir/fs::path("coef.txt"));
            vol << min.mean << '\t' << min.coef1 << '\t' << min.coef2
                << '\t' << max.mean << '\t' << max.coef1 << '\t' << max.coef2 << endl;
        }
        fs::ofstream html(dir/fs::path("index.html"));
        html << "<html><body>" << endl;
        html << "<table border=\"1\"><tr><th>Study</th><th>Sex</th><th>Age</th></tr>"
             << "<tr><td>" << study.dir().native() << "</td><td>" << (study.front().front().meta[Meta::SEX] ? "Female": "Male")
             << "</td><td>" << study.front().front().meta[Meta::AGE]
             << "</td></tr></table>" << endl;
        html << "<br/><img src=\"radius.png\"></img>" << endl;
        html << "<br/><table border=\"1\">"<< endl;
        html << "<tr><th>Slice</th><th>Location</th><th>Tscore</th><th>Pscore</th><th>image</th></tr>";
        fs::path gp1(dir/fs::path("plot.gp"));
        fs::ofstream gp(gp1);
        gp << "set xlabel \"time\";" << endl;
        gp << "set ylabel \"location\";" << endl;
        gp << "set zlabel \"radius\";" << endl;
        gp << "set hidden3d;" << endl;
        gp << "set style data pm3d;" << endl;
        gp << "set dgrid3d 50,50 qnorm 2;" << endl;
        gp << "splot '-' using 1:2:3 notitle" << endl;
        if (do_gif) {
#pragma omp parallel for
            for (unsigned i = 0; i < study.size(); ++i) {
                study[i].visualize();
                study[i].save_gif(dir/fs::path(fmt::format("{}.gif", i)));
            }
        }
        for (unsigned i = 0; i < study.size(); ++i) {
            auto &ss = study[i];
            namespace ba = boost::accumulators;
            typedef ba::accumulator_set<double, ba::stats<ba::tag::mean, ba::tag::min, ba::tag::max>> Acc;
            Acc acc1;
            Acc acc2;
            for (auto const &s: ss) {
                acc1(s.top_score);
                acc2(s.polar_score);
                float r = std::sqrt(s.box.area())/2 * s.meta.spacing;
                gp << s.meta.trigger_time
                   << '\t' << s.meta.slice_location
                   << '\t' << r << endl;
            }
            html << "<tr>"
                 << "<td>" << study[i].dir().filename().native() << "</td>"
                 << "<td>" << study[i].front().meta.slice_location << "</td>"
            //     << "<td>" << study[i].front().meta[Meta::NOMINAL_INTERVAL] << "</td>"
                 << "<td>" << ba::min(acc1) << "<br/>" << ba::mean(acc1) << "<br/>" << ba::max(acc1) << "</td>"
                 << "<td>" << ba::min(acc2) << "<br/>" << ba::mean(acc2) << "<br/>" << ba::max(acc2) << "</td>"
                 << "<td><img src=\"" << i << ".gif\"></img></td></tr>" << endl;
        }
        gp << 'e' << endl;
        html << "</table></body></html>" << endl;
        fs::ofstream os(dir/fs::path("report.txt"));
        for (auto const &series: study) {
            for (auto const &s: series) {
                report(os, s, bound);
            }
        }
        {
            fs::path gp2(dir/fs::path("plot2.gp"));
            fs::ofstream gp(gp2);
            gp << "set terminal png;" << endl;
            gp << "set output \"" << (dir/fs::path("radius.png")).native() << "\";" << endl;
            gp << "load \"" << gp1.native() << "\";" << endl;
            gp.close();
            string cmd = fmt::format("gnuplot {}", gp2.string());
            ::system(cmd.c_str());
            fs::remove(gp2);
        }
    }
    else {
        for (auto const &series: study) {
            for (auto const &s: series) {
                report(cout, s, bound);
            }
        }
    }
    return 0;
}

