#include <sstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <glog/logging.h>
#include "adsb2.h"

using namespace std;
using namespace adsb2;

static inline float sqr (float x) {
    return x * x;
}

void GaussianAcc (float v, float scale, vector<float> *s) {
    s->resize(Eval::VALUES);
    float sum = 0;
    for (unsigned i = 0; i < Eval::VALUES; ++i) {
        float x = (float(i) - v) / scale;
        x = exp(-0.5 * x * x);
        s->at(i) = x;
        sum += x;
    }
    float acc = 0;
    for (auto &v: *s) {
        acc += v;
        v = acc / sum;
    }
}

bool compute1 (StudyReport const &rep, float *sys, float *dia) {
    vector<float> all(rep[0].size(), 0);
    for (unsigned i = 1; i < rep.size(); ++i) {
        if (rep[i].size() != rep[0].size()) return false;
    }
    for (unsigned sl = 0; sl < all.size(); ++sl) {
        float v = 0;
        for (unsigned sax = 0; sax + 1 < rep.size(); ++sax) {

            float a = rep[sax][sl].data[SL_AREA] * sqr(rep[sax][sl].meta.spacing);
            float b = rep[sax+1][sl].data[SL_AREA] * sqr(rep[sax+1][sl].meta.spacing);
            float gap = fabs(rep[sax+1][sl].meta.slice_location
                      - rep[sax][sl].meta.slice_location);
            if (gap > 25) gap = 10;
            v += (a + b + sqrt(a*b)) * gap / 3;
        }
        all[sl] = v / 1000;
    }
    float m = all[0];
    float M = all[0];
    for (unsigned i = 1; i < all.size(); ++i) {
        if (all[i] < m) m = all[i];
        if (all[i] > M) M = all[i];
    }
    if (M < all[0] * 1.2) M = all[0];
    *dia = M;
    *sys = m;
    return true;
}

void compute2 (StudyReport const &rep, float *sys, float *dia) {
    float oma = 0, oMa = 0, ol = 0;
    float m = 0, M = 0;
    for (unsigned i = 0; i < rep.size(); ++i) {
        auto const &ss = rep[i];
        float ma = ss[0].data[SL_AREA] * sqr(ss[0].meta.spacing);
        float Ma = ma;
        for (auto const &s: ss) {
            float x = s.data[SL_AREA] * sqr(s.meta.spacing);
            if (x < ma) ma = x;
            if (x > Ma) Ma = x;
        }
        float l = ss[0].meta.slice_location;
        if (i > 0) {
            float gap = abs(ol - l);
            if (gap > 25) gap = 10;
            m += (oma + ma + sqrt(oma * ma)) * gap/3;
            M += (oMa + Ma + sqrt(oMa * Ma)) * gap/3;
        }
        oma = ma;
        oMa = Ma;
        ol = l;
    }
    *sys = m/1000;
    *dia = M/1000;
}

struct Sample {
    vector<float> ft;
    float sys, dia;
    float sys_p, dia_p;
    float sys_e, dia_e;
};

int main(int argc, char **argv) {
    //Series stack("sax", "tmp");
    namespace po = boost::program_options; 
    string config_path;
    vector<string> overrides;
    vector<string> paths;
    float scale;

    po::options_description desc("Allowed options");
    desc.add_options()
    ("help,h", "produce help message.")
    ("config", po::value(&config_path)->default_value("adsb2.xml"), "config file")
    ("override,D", po::value(&overrides), "override configuration.")
    ("input,i", po::value(&paths), "")
    ("scale,s", po::value(&scale)->default_value(20), "")
    ("detail", "")
    ;

    po::positional_options_description p;
    p.add("input", -1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
                     options(desc).positional(p).run(), vm);
    po::notify(vm); 

    if (vm.count("help") || paths.empty()) {
        cerr << desc;
        return 1;
    }
    bool detail = vm.count("detail") > 0;

    Config config;
    try {
        LoadConfig(config_path, &config);
    } catch (...) {
        cerr << "Failed to load config file: " << config_path << ", using defaults." << endl;
    }
    OverrideConfig(overrides, &config);
    GlobalInit(argv[0], config);
    Eval eval;
    vector<float> v;
    Classifier *target_sys = Classifier::get("target.sys");
    Classifier *target_dia = Classifier::get("target.dia");
    Classifier *error_sys = Classifier::get("error.sys");
    Classifier *error_dia = Classifier::get("error.dia");
    for (auto const &s: paths) {
        fs::path p(s);
        StudyReport x(p);
        float sys1, dia1, sys2, dia2;
        float gs_sys, gs_dia;
        int study = x.front().front().study_id;
        if (detail) {
            for (auto &s: x.back()) {
                s.data[SL_AREA] = 0;
            }
        }
        compute2(x, &sys2, &dia2);
        if (!compute1(x, &sys1, &dia1)) {
            sys1 = sys2;
            dia1 = dia2;
        }
        vector<float> ft{sys1, dia1, sys2, dia2, x[0][0].meta[Meta::SEX], x[0][0].meta[Meta::AGE]};

        float sys_mu = target_sys->apply(ft);
        float sys_sigma = 14; // sqrt(error_sys->apply(ft));
        float dia_mu = target_dia->apply(ft);
        float dia_sigma = 16; //sqrt(error_dia->apply(ft));

        GaussianAcc(sys_mu, sys_sigma, &v);
        cout << study << "_Systole" << '\t' << eval.score(study, 0, v) << endl;
        GaussianAcc(dia_mu, dia_sigma, &v);
        cout << study << "_Diastole" << '\t' << eval.score(study, 1, v) << endl;
    }
}

