#include <thread>
#include <mutex>
#include <unordered_map>
#define BOOST_SPIRIT_THREADSAFE
#include <boost/property_tree/xml_parser.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/moment.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <snappystream.hpp>
#include "adsb2.h"
#include "adsb2-io.h"

extern "C" {
void    openblas_set_num_threads (int);    
}

namespace adsb2 {
    char const *MetaBase::FIELDS[] = {
        "Sex",
        "Age",
        "SliceThickness",
        "NominalInterval",
        "CardiacNumberOfImages",
        "SliceLocation",
        "SeriesNumber"
    };

    using std::unordered_map;
    void LoadConfig (string const &path, Config *config) {
        try {
            boost::property_tree::read_xml(path, *config);
        }
        catch (...) {
            LOG(WARNING) << "Cannot load config file " << path << ", using defaults.";
        }
    }

    void SaveConfig (string const &path, Config const &config) {
        boost::property_tree::write_xml(path, config);
    }

    void OverrideConfig (std::vector<std::string> const &overrides, Config *config) {
        for (std::string const &D: overrides) {
            size_t o = D.find("=");
            if (o == D.npos || o == 0 || o + 1 >= D.size()) {
                std::cerr << "Bad parameter: " << D << std::endl;
                BOOST_VERIFY(0);
            }
            config->put<std::string>(D.substr(0, o), D.substr(o + 1));
        }
    }


    fs::path home_dir;
    fs::path temp_dir;
    fs::path model_dir;
    int caffe_batch = 0;
    int font_height = 0;
    int font_face = cv::FONT_HERSHEY_SIMPLEX;
    double font_scale = 0.4;
    int font_thickness = 1;
    cv::Mat polar_morph_kernel;

    void dicom_setup (char const *path, Config const &config);
    void GlobalInit (char const *path, Config const &config) {
        if (config.get<int>("adsb2.about", 0)) {
            std::cerr << "ADSB2 VERSION: " << VERSION << std::endl;
        }
#ifdef CPU_ONLY
        caffe_batch = 1;
#else
        caffe_batch = config.get<int>("adsb2.caffe.batch", 32);
#endif
        FLAGS_logtostderr = 1;
        FLAGS_minloglevel = config.get<int>("adsb2.log.level",1);
        home_dir = fs::path(path).parent_path();
        temp_dir = fs::path(config.get("adsb2.tmp_dir", "/tmp"));
        model_dir = fs::path(config.get("adsb2.models", (home_dir/fs::path("models")).native()));
        google::InitGoogleLogging(path);
        dicom_setup(path, config);
        //openblas_set_num_threads(config.get<int>("adsb2.threads.openblas", 1));
        cv::setNumThreads(config.get<int>("adsb2.threads.opencv", 1));
        int baseline = 0;
        cv::Size fsz = cv::getTextSize("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789", font_face, font_scale, font_thickness, &baseline);
        font_height = 14 * fsz.height / 10;
        polar_morph_kernel = cv::Mat::ones(3, 3, CV_8U);

        //Eval::CASES = config.get<int>("adsb2.eval.cases", 500);
    }

    void draw_text (cv::Mat &img, string const &text, cv::Point org, int line, cv::Scalar v) {
        org.y += (line + 1) * font_height;
        cv::putText(img, text, org, font_face, font_scale, v, font_thickness);
    }

    struct pairhash {
    public:
      template <typename T, typename U>
      std::size_t operator()(const std::pair<T, U> &x) const
      {
        return std::hash<T>()(x.first) ^ std::hash<U>()(x.second);
      }
    };

    template <typename T>
    class ModelManager {
        unordered_map<std::pair<std::thread::id, string>, T *, pairhash> insts;   // instance for each thread
        std::mutex mutex;
    public:
        ModelManager () {
        }

        ~ModelManager () {
            for (auto &p: insts) {
                CHECK(p.second);
                delete p.second;
            }
        }

        T *get (string const &name) {
            std::lock_guard<std::mutex> lock(mutex); 
            std::pair<std::thread::id, string> key(std::this_thread::get_id(), name);
            auto it = insts.find(key);
            if (it == insts.end()) {
                T *det = T::create(model_dir / fs::path(name));
                CHECK(det) << "failed to create detector " << name;
                insts[key] = det;
                return det;
            }
            else {
                return it->second;
            }
        }
    };

    ModelManager<Detector> detector_manager;
    ModelManager<Classifier> classifier_manager;

    Detector *Detector::get (string const &name) {
        return detector_manager.get(name);
    }

    Classifier *Classifier::get (string const &name) {
        return classifier_manager.get(name);
    }

    fs::path temp_path (const fs::path& model) {
        return fs::unique_path(temp_dir / model);
    }


    BoxAnnoOps box_anno_ops;
    PolyAnnoOps poly_anno_ops;
    PredAnnoOps pred_anno_ops;

#define BOX_IS_RATIO    1
    
    void BoxAnnoOps::load (Slice *slice, string const *txt) const
    {
        slice->anno = this;
        Data &box = slice->anno_data.box;
        box.x = lexical_cast<float>(txt[0]);
        box.y = lexical_cast<float>(txt[1]);
        box.width = lexical_cast<float>(txt[2]);
        box.height = lexical_cast<float>(txt[3]);
#ifdef BOX_IS_RATIO
        CHECK(box.x <= 1);
        CHECK(box.x + box.width <= 1);
        CHECK(box.y <= 1);
        CHECK(box.y + box.height <= 1);
#else
        CHECK(box.width >= 1);
        CHECK(box.height >= 1);
        CHECK(box.x + box.width > 1);
        CHECK(box.y + box.height > 1);
#endif
    }

    void BoxAnnoOps::shift (Slice *slice, cv::Point_<float> const &pt) const {
#ifdef BOX_IS_RATIO
        CHECK(0);
#else
        Data &box = slice->anno_data.box;
        box.x -= pt.x;
        box.y -= pt.y;
#endif
    }

    void BoxAnnoOps::scale (Slice *slice, float rate) const {
#ifdef BOX_IS_RATIO
#else
        Data &box = slice->anno_data.box;
        box.x *= rate;
        box.y *= rate;
        box.width *= rate;
        box.height *= rate;
#endif
    }

    void BoxAnnoOps::fill (Slice const &slice, cv::Mat *out, cv::Scalar const &v) const
    {
        cv::Rect_<float> box = slice.anno_data.box;
#ifdef BOX_IS_RATIO
        cv::Size sz = slice.images[IM_IMAGE].size();
        box.x *= sz.width;
        box.width *= sz.width;
        box.y *= sz.height;
        box.height *= sz.height;
#endif
        *out = cv::Mat(slice.images[IM_IMAGE].size(), CV_8U, cv::Scalar(0));
//#define BOX_AS_CIRCLE 1
#ifdef BOX_AS_CIRCLE
        cv::circle(*out, round(cv::Point_<float>(box.x + box.width/2, box.y + box.height/2)),
                         std::sqrt(box.width * box.height)/2, v, CV_FILLED);
        // TODO, use rotated rect to draw ellipse 
#else
        cv::rectangle(*out, round(box), v, CV_FILLED);
#endif
    }

    void BoxAnnoOps::contour (Slice const &slice, cv::Mat *out, cv::Scalar const &v) const
    {
        CHECK(0);
        Data const &box = slice.anno_data.box;
        *out = cv::Mat(slice.images[IM_IMAGE].size(), CV_32F, cv::Scalar(0));
#ifdef BOX_AS_CIRCLE
        cv::circle(*out, round(cv::Point_<float>(box.x + box.width/2, box.y + box.height/2)),
                         std::sqrt(box.width * box.height)/2, v);
        // TODO, use rotated rect to draw ellipse 
#else
        cv::rectangle(*out, round(box), v);
#endif
    }

    void PolyAnnoOps::load (Slice *slice, string const *txt) const
    {
        slice->anno = this;
        Data &poly = slice->anno_data.poly;
        poly.R = lexical_cast<float>(txt[0]);
        poly.C.x = lexical_cast<float>(txt[1]);
        poly.C.y = lexical_cast<float>(txt[2]);
        int n = lexical_cast<float>(txt[3]);
        int off = 4;
        poly.contour.clear();
        for (int i = 0; i < n; ++i) {
            poly.contour.emplace_back(lexical_cast<float>(txt[off]),
                                      lexical_cast<float>(txt[off+1]));
            off += 2;
        }
    }

    void PolyAnnoOps::shift (Slice *slice, cv::Point_<float> const &pt) const {
        Data &poly = slice->anno_data.poly;
        poly.C += pt;
    }

    void PolyAnnoOps::scale (Slice *slice, float rate) const {
        Data &poly = slice->anno_data.poly;
        poly.C.x *= rate;
        poly.C.y *= rate;
        poly.R *= rate;
    }

    void PolyAnnoOps::fill (Slice const &slice, cv::Mat *label, cv::Scalar const &v) const
    {
        int xx = 0;
        auto const &image = slice.images[IM_IMAGE];
        auto const &anno = slice.anno_data.poly;
        auto const &cc = anno.contour;
        {
            float bx = cc.back().x;
            float by = 1 - cc.back().y;
            float fx = cc.front().x;
            float fy = cc.front().y;
            xx = std::round((by * fx + fy * bx) / (fy + by) * image.cols);
        }
        vector<cv::Point> ps(cc.size());
        for (unsigned i = 0; i < cc.size(); ++i) {
            auto const &from = cc[i];
            auto &to = ps[i];
            to.x = std::round(image.cols * from.x);
            to.y = std::round(image.rows * from.y);
        }
        ps.emplace_back(xx, image.rows - 1);
        ps.emplace_back(0, image.rows - 1);
        ps.emplace_back(0, 0);
        ps.emplace_back(xx, 0);
        cv::Point const *pps = &ps[0];
        int const nps = ps.size();
        cv::Mat polar(image.size(), CV_32F, cv::Scalar(0));
        cv::fillPoly(polar, &pps, &nps, 1, v);
        cv::Mat out;
        linearPolar(polar, &out, anno.C, anno.R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS + CV_WARP_INVERSE_MAP);
        out.convertTo(*label, CV_8U);
    }

    void PolyAnnoOps::contour (Slice const &slice, cv::Mat *label, cv::Scalar const &v) const {
        CHECK(0);
    }

    void PredAnnoOps::load (Slice *slice, string const *txt) const
    {
        slice->anno = this;
        Data &poly = slice->anno_data.pred;
        poly.R = lexical_cast<float>(txt[0]);
        poly.C.x = lexical_cast<float>(txt[1]);
        poly.C.y = lexical_cast<float>(txt[2]);
        poly.size.width = lexical_cast<int>(txt[3]);
        poly.size.height = lexical_cast<int>(txt[4]);
        int off = 5;
        poly.contour.resize(poly.size.height);
        for (auto &x: poly.contour) {
            x = lexical_cast<int>(txt[off++]);
        }
    }

    void PredAnnoOps::shift (Slice *slice, cv::Point_<float> const &pt) const {
        Data &poly = slice->anno_data.pred;
        poly.C += pt;
    }

    void PredAnnoOps::scale (Slice *slice, float rate) const {
        Data &poly = slice->anno_data.pred;
        poly.C.x *= rate;
        poly.C.y *= rate;
        poly.R *= rate;
    }

    void PredAnnoOps::fill (Slice const &slice, cv::Mat *label, cv::Scalar const &v) const
    {
        auto const &anno = slice.anno_data.pred;
        cv::Mat polar(anno.size, CV_32F, cv::Scalar(0));
        auto const &cc = anno.contour;
        CHECK(cc.size() == polar.rows);
        for (int y = 0; y < polar.rows; ++y) {
            float *row = polar.ptr<float>(y);
            for (int x = 0; x < cc[y]; ++x) {
                row[x] = v[0];
            }
        }
        cv::Mat tmp;
        linearPolar(polar, &tmp, slice.images[IM_IMAGE].size(), anno.C, anno.R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS+CV_WARP_INVERSE_MAP);
        tmp.convertTo(*label, CV_8U);
        cv::morphologyEx(*label, *label, cv::MORPH_CLOSE, polar_morph_kernel);
    }

    void PredAnnoOps::contour (Slice const &slice, cv::Mat *out, cv::Scalar const &v) const
    {
        auto const &anno = slice.anno_data.pred;
        cv::Mat polar(anno.size, CV_32F, cv::Scalar(0));
        auto const &cc = anno.contour;
        CHECK(cc.size() == polar.rows);
        for (int y = 0; y < polar.rows; ++y) {
            float *row = polar.ptr<float>(y);
            row[cc[y]-1] = v[0];
            row[cc[y]] = v[0];
            row[cc[y]+1] = v[0];
        }
        linearPolar(polar, out, slice.images[IM_IMAGE].size(), anno.C, anno.R, CV_INTER_LINEAR+CV_WARP_FILL_OUTLIERS+CV_WARP_INVERSE_MAP);
    }

    Slice::Slice (string const &txt)
        : do_not_cook(false),
        box(-1,-1,0,0)
    {
        std::fill(data.begin(), data.end(), 0);
        using namespace boost::algorithm;
        line = txt;
        vector<string> ss;
        split(ss, line, is_any_of("\t"), token_compress_off);
        path = fs::path(ss[0]);

        string const *rest = &ss[1];
        int nf = ss.size() - 1;

        if (nf == 3) {
            LOG(ERROR) << "annotation format not supported.";
            CHECK(0);
        }
        else if (nf == 4) {
            box_anno_ops.load(this, rest);
        }
        else if (nf == 5) {
            LOG(ERROR) << "annotation format not supported.";
            CHECK(0);
        }
        else if (nf >= 7) {
            if (rest[0] == "pred") {
                pred_anno_ops.load(this, rest + 1);
            }
            else {
                poly_anno_ops.load(this, rest);
            }
        }
        else {
            LOG(ERROR) << "annotation format not supported.";
            CHECK(0);
        }
    }

    void Slice::save (std::ostream &os) const {
        int v = VERSION;
        io::write(os, v);
        io::write(os, id);
        io::write(os, path);
        io::write(os, meta);
        for (unsigned i = 0; i < IM_SIZE; ++i) {
            io::write(os, images[i]);
        }
        io::write(os, data);
        io::write(os, do_not_cook);
        io::write(os, line);

        int anno_id = 0;
        if (anno == nullptr) anno_id = 0;
        else if (anno == &box_anno_ops) anno_id = 1;
        else if (anno == &poly_anno_ops) anno_id = 2;
        else if (anno == &pred_anno_ops) anno_id = 3;
        else CHECK(0) << "unknown annotation type.";
        io::write(os, anno_id);

        anno_data.save(os);
        io::write(os, polar_C);
        io::write(os, polar_R);
        io::write(os, polar_contour);
        io::write(os, polar_box);
        io::write(os, local_box);
        io::write(os, box);
        io::write(os, _extra);
    }

    void Slice::load (std::istream &is) {
        int v;
        io::read(is, &v);
        CHECK(v <= VERSION);
        io::read(is, &id);
        io::read(is, &path);
        io::read(is, &meta);
        for (unsigned i = 0; i < IM_SIZE; ++i) {
            io::read(is, &images[i]);
        }
        if (v == 1) {
            is.read(reinterpret_cast<char *>(&data[0]),
                    sizeof(data[0]) * (SL_SIZE -1));
            data[SL_XA] = 0;
        }
        else {
            io::read(is, &data);
        }
        io::read(is, &do_not_cook);
        io::read(is, &line);

        int anno_id = 0;
        io::read(is, &anno_id);
        if (anno_id == 0) anno = nullptr;
        else if (anno_id == 1) anno = &box_anno_ops;
        else if (anno_id == 2) anno = &poly_anno_ops;
        else if (anno_id == 3) anno = &pred_anno_ops;
        else CHECK(0) << "unknown annotation type.";

        anno_data.load(is);
        io::read(is, &polar_C);
        io::read(is, &polar_R);
        io::read(is, &polar_contour);
        io::read(is, &polar_box);
        io::read(is, &local_box);
        io::read(is, &box);
        io::read(is, &_extra);
    }


    void Slice::clone (Slice *s) const {
        s->id = id;
        s->path = path;
        s->meta = meta;
        s->data = data;
        for (unsigned i = 0; i < IM_SIZE; ++i) {
            s->images[i] = images[i].clone();
        }
        s->do_not_cook = do_not_cook;
        s->line = line;
        s->anno = anno;
        s->anno_data = anno_data;
        s->box = box;
    }

    void Slice::visualize (bool show_prob) {
        cv::Scalar color(0xFF);
        cv::Mat image = images[IM_IMAGE];
        CHECK(image.type() == CV_32FC1);
        if (box.x >= 0) {
            cv::rectangle(image, box, color);
        }
        if (show_prob && images[IM_PROB].data) {
            cv::Mat pp;
            cv::normalize(images[IM_PROB], pp, 0, 255, cv::NORM_MINMAX, CV_32F);
#if 0
            cv::normalize(prob, pp, 0, 255, cv::NORM_MINMAX, CV_8U);
            cv::cvtColor(pp, rgb, CV_GRAY2BGR);
            if (box.x >= 0) {
                cv::rectangle(rgb, box, color);
            }
            cv::hconcat(image, rgb, image);
#else
            if (box.x >= 0) {
                cv::rectangle(pp, box, color);
            }
            cv::hconcat(image, pp, image);
#endif
        }
        if (_extra.data) {
            cv::hconcat(image, _extra, image);
        }
        image.convertTo(images[IM_VISUAL], CV_8U);
        cv::Point org(images[IM_IMAGE].cols + 20, 0);
        draw_text(images[IM_VISUAL], fmt::format("AR: {:3.2f}", data[SL_AREA]), org, 0);
        draw_text(images[IM_VISUAL], fmt::format("BS: {:1.2f}", data[SL_BSCORE]), org, 1);
        if (data[SL_BSCORE_DELTA]) {
            draw_text(images[IM_VISUAL], fmt::format("BD: {:1.2f}", data[SL_BSCORE_DELTA]), org, 2);
        }
        draw_text(images[IM_VISUAL], fmt::format("PS: {:1.2f}", data[SL_PSCORE]), org, 3);
        draw_text(images[IM_VISUAL], fmt::format("CR: {:1.2f}", data[SL_CSCORE]), org, 4);
        draw_text(images[IM_VISUAL], fmt::format("TS: {:1.2f}", data[SL_TSCORE]), org, 5);
        draw_text(images[IM_VISUAL], fmt::format("BT: {:1.2f}", data[SL_BOTTOM]), org, 6);
        draw_text(images[IM_VISUAL], fmt::format("CS: {:2.1f}", data[SL_CCOLOR]), org, 7);
        draw_text(images[IM_VISUAL], fmt::format("BP: {:1.1f}", data[SL_BOTTOM_PATCH]), org, 8);
        draw_text(images[IM_VISUAL], fmt::format("XA: {:3.2f}", data[SL_XA]), org, 9);
    }

    void Slice::update_polar (cv::Point_<float> const &C, float R) {
        polar_C = C;
        polar_R = R;
        linearPolar(images[IM_IMAGE], &images[IM_POLAR], polar_C, polar_R, CV_INTER_LINEAR+CV_WARP_FILL_OUTLIERS);
#if 0
        if (!det) {
            polar_prob = polar;
        }
        else {
            cv::Mat u8;
            polar.convertTo(u8, CV_8U);
            //equalizeHist(u8, u8);
            int m = polar.rows / 4;
            cv::Mat extended;
            vconcat3(u8.rowRange(polar.rows - m, polar.rows),
                     u8,
                     u8.rowRange(0, m),
                     &extended);
            cv::Mat extended_prob;
            det->apply(extended, &extended_prob);
            polar_prob = extended_prob.rowRange(m, m + polar.rows).clone();
            polar_prob *= 255;
        }
#endif
    }

    void Slice::update_local (cv::Rect const &l) {
        local_box = l;
        images[IM_LOCAL] = images[IM_IMAGE](local_box).clone();
    }

#if 0
    void Slice::eval (cv::Mat mat, float *s1, float *s2) const {
        CHECK(box.x >=0 && box.y >= 0);
        cv::Mat roi = mat(round(box));
        float total = cv::sum(mat)[0];
        float covered = cv::sum(roi)[0];
        *s1 = covered / total;
        *s2 = 0;//*s2 = std::sqrt(roi.area()) * meta.spacing;
    }
#endif

    Series::Series (fs::path const &path_, bool load, bool check, bool fix): path(path_) {
        // enumerate DCM files
        vector<fs::path> paths;
        fs::directory_iterator end_itr;
        for (fs::directory_iterator itr(path);
                itr != end_itr; ++itr) {
            if (fs::is_regular_file(itr->status())) {
                // found subdirectory,
                // create tagger
                auto path = itr->path();
                auto ext = path.extension();
                if (ext.string() != ".dcm") {
                    LOG(WARNING) << "Unknown file type: " << path.string();
                    continue;
                }
                paths.push_back(path);
            }
        }
        CHECK(paths.size());
        std::sort(paths.begin(), paths.end());
        resize(paths.size());
        for (unsigned i = 0; i < paths.size(); ++i) {
            Slice &s = at(i);
            s.path = paths[i];
            if (load) {
                s.load_raw();
                if (i) {
                    CHECK(s.meta.spacing == at(0).meta.spacing);
                    CHECK(s.images[IM_IMAGE].size() == at(0).images[IM_IMAGE].size());
                }
            }
        }
        if (load && check && !sanity_check(fix) && fix) {
            CHECK(sanity_check(false));
        }
    }

    void Series::shrink (cv::Rect const &bb) {
#if 0
        CHECK(size());
        CHECK(bb.x >= 0);
        CHECK(bb.y >= 0);
        CHECK(bb.width < front().image.cols);
        CHECK(bb.height < front().image.rows);
        cv::Mat vimage = front().vimage(bb).clone();
        for (Slice &s: *this) {
            s.image = s.image(bb).clone();
            s.vimage = vimage;
            if (s.anno) {
                s.anno->shift(&s, unround(bb.tl()));
            }
            CHECK(s.box.x < 0);
            CHECK(s.box.y < 0);
            CHECK(!s.prob.data);
        }
#endif
    }

    void Series::save_dir (fs::path const &dir, fs::path const &ext) {
        fs::create_directories(dir);
        for (auto const &s: *this) {
            CHECK(s.images[IM_VISUAL].depth() == CV_8U) << "image not suitable for visualization, call visualize() first";
            fs::path path(dir / s.path.stem());
            path += ext;
            cv::imwrite(path.native(), s.images[IM_VISUAL]);
        }
    }

    void Series::save_gif (fs::path const &path, int delay) {
        fs::path tmp(temp_path());
        fs::create_directories(tmp);
        ostringstream gif_cmd;
        gif_cmd << "convert -delay " << delay << " ";
        fs::path pgm(".pgm");
        fs::path pbm(".pbm");
        int cc = 0;
        for (auto const &s: *this) {
            cv::Mat visual = s.images[IM_VISUAL];
            CHECK(visual.data && visual.depth() == CV_8U) << "image not suitable for visualization, call visualize() first";
            fs::path pnm(tmp / fs::path(lexical_cast<string>(cc++)));
            if (visual.channels() == 1) {
                pnm += pgm;
            }
            else if (visual.channels() == 3) {
                pnm += pbm;
            }
            else {
                CHECK(0) << "image depth not supported.";
            }
            cv::imwrite(pnm.native(), visual);
            gif_cmd << " " << pnm;
        }
        gif_cmd << " " << path;
        ::system(gif_cmd.str().c_str());
        fs::remove_all(tmp);
    }

    void Series::visualize (bool show_prob) {
        for (Slice &s: *this) {
            s.visualize(show_prob);
        }
    }

    void Series::getVarImageRaw (cv::Mat *vimage) {
        cv::Mat first = front().images[IM_RAW];
        if (size() <= 1) {
            *vimage = cv::Mat(first.size(), CV_32F, cv::Scalar(0));
            return;
        }
        namespace ba = boost::accumulators;
        typedef ba::accumulator_set<double, ba::stats<ba::tag::mean, ba::tag::min, ba::tag::max, ba::tag::count, ba::tag::variance, ba::tag::moment<2>>> Acc;
        CHECK(size());

        cv::Size shape = first.size();
        unsigned pixels = shape.area();

        cv::Mat mu(shape, CV_32F);
        cv::Mat sigma(shape, CV_32F);
        vector<Acc> accs(pixels);
        for (auto &s: *this) {
            cv::Mat &raw = s.images[IM_RAW];
            CHECK(raw.type() == CV_16U);
            CHECK(raw.isContinuous());
            uint16_t const *v = raw.ptr<uint16_t const>(0);
            for (auto &acc: accs) {
                acc(*v);
                ++v;
            }
        }
        float *m = mu.ptr<float>(0);
        float *s = sigma.ptr<float>(0);
        //float *sp = spread.ptr<float>(0);
        for (auto const &acc: accs) {
            *m = ba::mean(acc);
            *s = std::sqrt(ba::variance(acc));
            //cout << *s << endl;
            //*sp = ba::max(acc) - ba::min(acc);
            ++m; ++s; //++sp;
        }
        *vimage = sigma;
    }

    template <typename T>
    class FreqCount {
        unordered_map<T, unsigned> cnt;
    public:
        void update (T const &v) {
            cnt[v] += 1;
        }
        bool unique () const {
            return cnt.size() <= 1;
        }
        T most_frequent () const {
            auto it = std::max_element(cnt.begin(), cnt.end(),
                    [](std::pair<T, unsigned> const &p1,
                       std::pair<T, unsigned> const &p2) {
                        return p1.second < p2.second;
                    });
            float mfv = it->first;
        }
    };

    bool operator < (Slice const &s1, Slice const &s2)
    {
        return s1.meta.trigger_time < s2.meta.trigger_time;
    }

    bool Series::sanity_check (bool fix) {
        bool ok = true;
        for (Slice &s: *this) {
            if (s.images[IM_RAW].size() != front().images[IM_RAW].size()) {
                LOG(ERROR) << "image size mismatch: " << s.path;
            }
            if (s.meta[Meta::NUMBER_OF_IMAGES] != size()) {
                ok = false;
                LOG(WARNING) << "Series field #images mismatch: " << s.path << " found " << s.meta[Meta::NUMBER_OF_IMAGES] << " instead of actually # images found " << size();
                if (fix) {
                    s.meta[Meta::NUMBER_OF_IMAGES] = size();
                }
            }
        }
        for (unsigned i = 0; i < Meta::SERIES_FIELDS; ++i) {
            // check that all series fields are the same
            FreqCount<float> fc;
            for (Slice &s: *this) {
                fc.update(s.meta[i]);
            }
            if (fc.unique()) break;
            ok = false;
            float mfv = fc.most_frequent();
            for (Slice &s: *this) {
                if (s.meta[i] != mfv) {
                    LOG(WARNING) << "Series field " << Meta::FIELDS[i] << "  mismatch: " << s.path << " found " << s.meta[i]
                                 << " instead of most freq value " << mfv;
                    if (fix) {
                        s.meta[i] = mfv;
                    }
                }
            }
        }
        // check trigger time
        bool ooo = false;
        for (unsigned i = 1; i < size(); ++i) {
            if (!(at(i).meta.trigger_time >= at(i-1).meta.trigger_time)) {
                ooo = true;
                ok = false;
                LOG(WARNING) << "Trigger time out of order: "
                             << at(i-1).path << ':' << at(i-1).meta.trigger_time
                             << " > "
                             << at(i).path << ':' << at(i).meta.trigger_time;
            }
        }
        if (fix) {
            sort(begin(), end());
        }
        return ok;
    }

    void Study::probe (fs::path const &path_, Meta *meta) {
        // enumerate DCM files
        vector<fs::path> paths;
        fs::directory_iterator end_itr;
        for (fs::directory_iterator itr(path_);
                itr != end_itr; ++itr) {
            if (fs::is_directory(itr->status())) {
                // found subdirectory,
                // create tagger
                auto sax = itr->path();
                string name = sax.filename().native();
                if (name.find("sax_") != 0) {
                    continue;
                }
                fs::directory_iterator end_itr2;
                for (fs::directory_iterator itr2(sax);
                        itr2 != end_itr2; ++itr2) {
                    if (fs::is_regular_file(itr->status())) {
                        // found subdirectory,
                        // create tagger
                        auto path2 = itr->path();
                        auto ext = path2.extension();
                        if (ext.string() != ".dcm") {
                            LOG(WARNING) << "Unknown file type: " << path2.string();
                            continue;
                        }
                        cv::Mat m = load_dicom (path2, meta);
                        if (m.data) return;
                    }
                }
            }
        }
        CHECK(0) << "no DCM file found/loaded.";
    }

    void Study::load_raw (fs::path const &path_, bool load, bool check, bool fix) {
        path = path_;
        // enumerate DCM files
        vector<fs::path> paths;
        fs::directory_iterator end_itr;
        for (fs::directory_iterator itr(path);
                itr != end_itr; ++itr) {
            if (fs::is_directory(itr->status())) {
                // found subdirectory,
                // create tagger
                auto sax = itr->path();
                string name = sax.filename().native();
                if (name.find("sax_") != 0) {
                    continue;
                }
                paths.push_back(sax);
            }
        }
        std::sort(paths.begin(), paths.end());
        CHECK(paths.size());
        for (auto const &sax: paths) {
            emplace_back(sax, load, false, false);    // do not fix for now
        }
        if (load && !sanity_check(fix) && fix) {
            CHECK(sanity_check(false));
        }
    }

    bool Study::detect_topdown (bool fix) {
        // some of the study have slice_location of a wrong sign
        // if sorted with slice_location the series will be in a wrong order
        vector<std::pair<int, float>> rank;
        for (auto const &ss: *this) {
            auto const &s = ss.front();
            rank.emplace_back(s.meta[Meta::SERIES_NUMBER], s.meta.slice_location);
        }
        sort(rank.begin(), rank.end());
        int good = 0;
        int bad = 0;
        for (unsigned i = 1; i < rank.size(); ++i) {
            if (rank[i].second > rank[i-1].second) ++good;
            else if (rank[i].second < rank[i-1].second) ++bad;
        }
        bool topdown = bad > good;
        if (topdown && fix) {
            LOG(WARNING) << "fixing slice " << path << " topdown";
            for (auto &ss: *this) {
                for (auto &s: ss) {
                    s.meta.slice_location = -s.meta.slice_location;
                }
            }
        }
        return topdown;
    }

    void Study::save (fs::path const &path) const {
        fs::ofstream os(path);
        if (!os.is_open()) return;
        snappy::oSnappyStream osnstrm(os);
        save(osnstrm);
    }

    void Study::load (fs::path const &path) {
        //using boost::iostreams;
        fs::ifstream is(path);
        if (!is.is_open()) return;
        snappy::iSnappyStream isnstrm(is);
        /*
        filtering_streambuf<input> in;
        in.push(gzip_decompressor());
        in.push(is);
        load(in);
        */
        load(isnstrm);
    }


    static constexpr float LOCATION_GAP_EPSILON = 0.01;
    static inline bool operator < (Series const &s1, Series const &s2) {
        Meta const &m1 = s1.front().meta;
        Meta const &m2 = s2.front().meta;
        if (m1.slice_location + LOCATION_GAP_EPSILON < m2.slice_location) {
            return true;
        }
        if (m1.slice_location - LOCATION_GAP_EPSILON > m2.slice_location) {
            return false;
        }
        return m1[Meta::SERIES_NUMBER] < m2[Meta::SERIES_NUMBER];
    }

    bool Study::sanity_check (bool fix) {
        bool ok = true;
        if (fix) {
            check_regroup();
        }
        detect_topdown(fix);
        cv::Size image_size = front().front().images[IM_RAW].size();
        for (auto &s: *this) {
            if (!s.sanity_check(fix)) {
                LOG(WARNING) << "Study " << path << " series " << s.path << " sanity check failed.";
                if (fix) {
                    CHECK(s.sanity_check(false));
                }
            }
            if (s.front().images[IM_RAW].size() != image_size) {
                LOG(ERROR) << "Study " << path << " series " << s.path << " image size mismatch.";
            }
        }
        for (unsigned i = 0; i < Meta::STUDY_FIELDS; ++i) {
            // check that all series fields are the same
            FreqCount<float> fc;
            for (Series &s: *this) {
                fc.update(s.front().meta[i]);
            }
            if (fc.unique()) break;
            ok = false;
            float mfv = fc.most_frequent();
            for (Series &s: *this) {
                if (s.front().meta[i] != mfv) {
                    LOG(WARNING) << "Study field " << Meta::FIELDS[i] << "  mismatch: " << s.dir() << " found " << s.front().meta[i]
                                 << " instead of most freq value " << mfv;
                    if (fix) {
                        for (auto &ss: s) {
                            ss.meta[i] = mfv;
                        }
                    }
                }
            }
        }
        // check trigger time
        //
        sort(begin(), end());
        unsigned off = 1;
        for (unsigned i = 1; i < size(); ++i) {
            Meta const &prev = at(off-1).front().meta;
            Meta const &cur = at(i).front().meta;
            if (std::abs(prev.slice_location - cur.slice_location) <= LOCATION_GAP_EPSILON) {
                LOG(WARNING) << "replacing " << at(off-1).dir()
                             << " (" << prev[Meta::SERIES_NUMBER] << ":" << prev.slice_location << ") "
                             << " with " << at(i).dir()
                             << " (" << cur[Meta::SERIES_NUMBER] << ":" << cur.slice_location << ") ";
                std::swap(at(off-1), at(i));
            }
            else {
                if (off != i) { // otherwise no need to swap
                    std::swap(at(off), at(i));
                }
                ++off;
            }
        }
        if (off != size()) {
            LOG(WARNING) << "study " << path << " reduced from " << size() << " to " << off << " series.";
            resize(off);
        }
        return ok;
    }

    void Study::check_regroup () {
        vector<Series> v;
        v.swap(*this);
        for (Series &s: v) {
            unsigned max_nn = 0;
            unordered_map<float, vector<unsigned>> group;
            for (unsigned i = 0; i < s.size(); ++i) {
                auto const &ss = s[i];
                unsigned nn = ss.meta[Meta::NUMBER_OF_IMAGES];
                if (nn > max_nn) {
                    max_nn = nn;
                }
                group[ss.meta.slice_location].push_back(i);
            }
            if ((s.size() <= max_nn) && (group.size() <= 1)) {
                emplace_back(std::move(s));
            }
            else { // regroup
                LOG(WARNING) << "regrouping series " << s.dir() << " into " << group.size() << " groups.";
                unsigned i;
                for (auto const &p: group) {
                    emplace_back();
                    back().path = s.path;
                    back().path += fs::path(':' + lexical_cast<string>(i));;
                    for (unsigned j: p.second) {
                        back().push_back(std::move(s[j]));
                    }
                    sort(back().begin(), back().end());
                    ++i;
                }
            }
        }
    }

    // histogram equilization
#if 0
    void getColorMap (Series &series, vector<float> *cmap, int colors, uint16_t *lb, uint16_t *ub) {
        vector<uint16_t> all;
        all.reserve(series.front().images[IM_RAW].total() * series.size());
        for (auto &s: series) {
            loop<uint16_t>(s.images[IM_RAW], [&all](uint16_t v) {
                all.push_back(v);
            });
        }
        sort(all.begin(), all.end());
        uint16_t lb1 = all[all.size() * 0.2];
        uint16_t ub1 = all[all.size() - all.size() * 0.05];
        cmap->resize(all.back()+1);
        unsigned b = 0;
        for (unsigned c = 0; c < colors; ++c) {
            if (b >= all.size()) break;
            unsigned e0 = all.size() * (c + 1) / colors;
            unsigned e = e0;
            // extend e0 to color all of the same color
            while ((e < all.size()) && (all[e] == all[e0])) ++e;
            unsigned cb = all[b];
            unsigned ce = (e < all.size()) ? all[e] : (all.back() + 1);
            // this is the last color bin
            // check that we have covered all colors
            if ((c+1 >= colors) && (ce != all.back() + 1)) {
                LOG(WARNING) << "bad color mapping";
                ce = all.back() + 1;
            }
            for (unsigned i = cb; i < ce; ++i) {
                cmap->at(i) = c;
            }
            b = e;
        }
        all.resize(unique(all.begin(), all.end()) - all.begin());
        uint16_t lb2 = all[all.size() * 0.005];
        uint16_t ub2 = all[all.size()-1 - all.size() * 0.2];
        *lb = std::max(lb1, lb2);
        *ub = std::min(ub1, ub2);
    }

    void equalize (cv::Mat from, cv::Mat *to, vector<float> const &cmap) {
        CHECK(from.type() == CV_16UC1);
        to->create(from.size(), CV_32FC1);
        for (int i = 0; i < from.rows; ++i) {
            uint16_t const *f = from.ptr<uint16_t const>(i);
            float *t = to->ptr<float>(i);
            for (int j = 0; j < from.cols; ++j) {
                t[j] = cmap[f[j]];
            }
        }
    }
#endif

    void getColorBounds (Series &series, float *lb, float *ub) {
        vector<uint16_t> all;
        all.reserve(series.front().images[IM_RAW].total() * series.size());
        for (auto &s: series) {
            loop<uint16_t>(s.images[IM_RAW], [&all](uint16_t v) {
                all.push_back(v);
            });
        }
        sort(all.begin(), all.end());
        uint16_t lb1 = all[all.size() * 0.2];
        uint16_t ub1 = all[all.size() - all.size() * 0.05];
        all.resize(unique(all.begin(), all.end()) - all.begin());
        uint16_t lb2 = all[all.size() * 0.005];
        uint16_t ub2 = all[all.size()-1 - all.size() * 0.2];

        *lb = std::max(lb1, lb2);
        *ub = std::min(ub1, ub2);
        if (*lb + GRAYS > *ub) {
            *ub = *lb + GRAYS;
        }
    }

    void Cook::apply (Slice *slice) const {
        //CHECK(0) << "Unimplemented";   // not supported yet
        string sax = slice->path.parent_path().native();
        auto it = cbounds.find(sax);
        CHECK(it != cbounds.end()) << " color bounds not found.";
        float lb = it->second.first;
        float ub = it->second.second;
        slice->data[SL_COLOR_LB] = lb;
        slice->data[SL_COLOR_UB] = ub;
        slice->images[IM_RAW].convertTo(slice->images[IM_IMAGE], CV_32F);
        scale_color(&slice->images[IM_IMAGE], lb, ub);

        if (spacing > 0) {
            float raw_spacing = slice->meta.raw_spacing;
            float scale = raw_spacing / spacing;
            cv::Size sz = round(slice->images[IM_RAW].size() * scale);
            slice->meta.spacing = spacing;
            //float scale = s.meta.raw_spacing / s.meta.spacing;
            cv::resize(slice->images[IM_IMAGE], slice->images[IM_IMAGE], sz);
            //cv::resize(s.images[IM_EQUAL], s.images[IM_EQUAL], sz);
            if (slice->anno) {
                slice->anno->scale(slice, scale);
            }
        }
    }

    void Cook::apply (Series *series) const {
        // normalize color
        cv::Mat vimage;
        series->getVarImageRaw(&vimage);
        cv::Size raw_size = vimage.size();
        // compute var image
        cv::normalize(vimage, vimage, 0, GRAYS-1, cv::NORM_MINMAX, CV_32F);
        float scale = -1;
        float raw_spacing = -1;
        cv::Size sz;
        if (spacing > 0) {
            //float scale = spacing / meta.spacing;
            raw_spacing = series->front().meta.raw_spacing;
            scale = raw_spacing / spacing;
            sz = round(raw_size * scale);
            cv::resize(vimage, vimage, sz);
        }
        float lb, ub;
        /*
        vector<float> cmap;
        getColorMap(*series, &cmap, color_bins, &lb, &ub);
        */
        getColorBounds(*series, &lb, &ub);
#pragma omp parallel for schedule(dynamic, 1)
        for (unsigned i = 0; i < series->size(); ++i) {
            auto &s = series->at(i);
            if (s.do_not_cook) continue;
            s.data[SL_COLOR_LB] = lb;
            s.data[SL_COLOR_UB] = ub;
            s.images[IM_RAW].convertTo(s.images[IM_IMAGE], CV_32F);
            scale_color(&s.images[IM_IMAGE], lb, ub);
            /*
            equalize(s.images[IM_RAW], &s.images[IM_EQUAL], cmap);
            */
            //s.images[IM_EQUAL] = s.images[IM_IMAGE].clone();
            if (scale > 0) {
                s.meta.spacing = spacing;
                CHECK(s.meta.raw_spacing == raw_spacing);
                //float scale = s.meta.raw_spacing / s.meta.spacing;
                cv::resize(s.images[IM_IMAGE], s.images[IM_IMAGE], sz);
                //cv::resize(s.images[IM_EQUAL], s.images[IM_EQUAL], sz);
                if (s.anno) {
                    s.anno->scale(&s, scale);
                }
            }
#pragma omp critical
            s.images[IM_VAR] = vimage;
        }
    }

    void Cook::apply (Study *study) const {
        for (auto &ss: *study) {
            apply(&ss);
        }
        cv::Size sz(0, 0);
        for (auto &ss: *study) {
            CHECK(ss.size());
            cv::Size ssz = ss[0].images[IM_IMAGE].size();
            if (ssz.width > sz.width) sz.width = ssz.width;
            if (ssz.height > sz.height) sz.height = ssz.height;
            for (unsigned i = 1; i < ss.size(); ++i) {
                CHECK(ss[i].images[IM_IMAGE].size() == ssz);
            }
        }
        for (auto &ss: *study) {
            if (ss[0].images[IM_IMAGE].size() == sz) continue;
            // otherwise expand all images in this one
            cv::Size ssz = ss[0].images[IM_IMAGE].size();
            int top = (sz.height - ssz.height) / 2;
            int bottom = (sz.height - ssz.height - top);
            int left = (sz.width - ssz.width) / 2;
            int right = (sz.width - ssz.width - left);
            LOG(WARNING) << "resizing series " << ss.dir() << " into " << sz.width << "x" << sz.height;
            cv::Mat vimage(sz, CV_32F);
            cv::copyMakeBorder(ss[0].images[IM_VAR], vimage, top, bottom, left, right, cv::BORDER_REPLICATE);
            for (auto &s: ss) {
                if (s.do_not_cook) continue;
                cv::Mat image(sz, CV_32F);
                cv::copyMakeBorder(s.images[IM_IMAGE], image, top, bottom, left, right, cv::BORDER_REPLICATE);
                s.images[IM_VAR] = vimage;
                s.images[IM_IMAGE] = image;
            }
        }
    }

    Slices::Slices (fs::path const &list_path, fs::path const &root, Cook const &cook) {
        fs::ifstream is(list_path);
        CHECK(is) << "Cannot open list file: " << list_path;
        string line;
        while (getline(is, line)) {
            Slice s(line);
            if (s.line.empty()) {
                LOG(ERROR) << "bad line: " << line;
                continue;
            }
            if (s.path.extension().string() != ".dcm") {
                LOG(ERROR) << "not DCM file: " << s.path;
                continue;
            }
            fs::path f = root;
            f /= s.path;
            if (!fs::is_regular_file(f)) {
                LOG(ERROR) << "not regular file: " << f;
                continue;
            }
            push_back(s);
        }
        // distribute samples to stacks
        std::unordered_map<string, vector<unsigned>> dirs;
        for (unsigned i = 0; i < size(); ++i) {
            dirs[at(i).path.parent_path().string()].push_back(i);
        }
        LOG(INFO) << "found files in " << dirs.size() << " dirs.";
    
        boost::progress_display progress(dirs.size(), std::cerr);
        vector<std::pair<fs::path, vector<unsigned>>> todo;
        for (auto &p: dirs) {
            todo.emplace_back(fs::path(p.first), std::move(p.second));
        }
#pragma omp parallel for
        for (unsigned ii = 0; ii < todo.size(); ++ii) {
            fs::path dir = root / fs::path(todo[ii].first);
            Series stack(dir);
            vector<std::pair<unsigned, unsigned>> offs;
            {
                unordered_map<string, unsigned> mm;
                for (unsigned i = 0; i < stack.size(); ++i) {
                    mm[stack[i].path.stem().string()] = i;
                }
                // offset mapping: from samples offset to stack offset
                for (unsigned i: todo[ii].second) {
                    auto it = mm.find(at(i).path.stem().string());
                    CHECK(it != mm.end()) << "cannot find " << at(i).path << " in dir " << dir;
                    offs.emplace_back(i, it->second);
                }
            }
            for (auto &s: stack) {
                s.do_not_cook = true;
            }
            for (auto const &p: offs) {
                Slice &from = at(p.first);
                Slice &to = stack[p.second];
                to.do_not_cook = false;
                to.line = from.line;
                to.anno = from.anno;
                to.anno_data = from.anno_data;
            }
            // move annotation data to stack
            cook.apply(&stack);
            // now extract the files we want
            for (auto const &p: offs) {
                std::swap(at(p.first), stack[p.second]);
            }
            ++progress;
        }
    }

    float accumulate (cv::Mat const &image, vector<float> *pX, vector<float> *pY) {
        vector<float> X(image.cols, 0);
        vector<float> Y(image.rows, 0);
        float total = 0;
        CHECK(image.type() == CV_32F);
        for (int y = 0; y < image.rows; ++y) {
            float const *row = image.ptr<float const>(y);
            for (int x = 0; x < image.cols; ++x) {
                float v = row[x];
                X[x] += v;
                Y[y] += v;
                total += v;
            }
        }
        pX->swap(X);
        pY->swap(Y);
        return total;
    }

    Eval::Eval () {
        fs::ifstream is(home_dir/fs::path("train.csv"));
        CHECK(is);
        string dummy;
        getline(is, dummy);
        int a;
        char d1, d2;
        E e;
        while (is >> a >> d1 >> e[0] >> d2 >> e[1]) {
            //CHECK(a == i+1);
            CHECK(d1 == ',');
            CHECK(d2 == ',');
            volumes[a] = e;
        }
    }

    float Eval::crps (float v, vector<float> const &x) {
        CHECK(x.size() == VALUES);
        float sum = 0;
        unsigned i = 0;
        for (; i < v; ++i) {
            float s = x[i];
            sum += s * s;
        }
        for (; i < VALUES; ++i) {
            float s = 1.0 - x[i];
            sum += s * s;
        }
        for (unsigned i = 0; i < VALUES; ++i) {
            CHECK(x[i] >= 0) << x[i];
            CHECK(x[i] <= 1) << x[i];
            if (i > 0) CHECK(x[i] >= x[i-1]);
        }
        /*
        CHECK(x.front() == 0);
        CHECK(x.back() == 1);
        */
        return sum/VALUES;
    }

    float Eval::score (fs::path const &path, vector<std::pair<string, float>> *s) {
        fs::ifstream is(path);
        string line;
        getline(is, line);
        float sum = 0;
        s->clear();
        while (getline(is, line)) {
            using namespace boost::algorithm;
            vector<string> ss;
            split(ss, line, is_any_of(",_"), token_compress_on);
            CHECK(ss.size() == VALUES + 2);
            string name = ss[0] + "_" + ss[1];
            int n = lexical_cast<int>(ss[0]);
            int m;
            if (ss[1] == "Systole") {
                m = 0;
            }
            else if (ss[1] == "Diastole") {
                m = 1;
            }
            else CHECK(0);
            float v = get(n,m);
            if (v < 0) LOG(ERROR) << "Cannot find training data for " << name;
            vector<float> x;
            for (unsigned i = 0; i < VALUES; ++i) {
                x.push_back(lexical_cast<float>(ss[2 + i]));
            }
            float score = crps(v, x);
            s->push_back(std::make_pair(name, score));
            sum += score;
        }
        CHECK(s->size());
        return sum / s->size();
    }
    float Eval::score (unsigned n1, unsigned n2, vector<float> const &x) {
        float v = get(n1, n2); //volumes[n1][n2];
        CHECK(v >= 0);
        return crps(v, x);
    }

    fs::path find24ch (fs::path const &root, string const &pat) {
        vector<fs::path> paths;
        fs::directory_iterator end_itr;
        for (fs::directory_iterator itr(root);
                itr != end_itr; ++itr) {
            if (fs::is_directory(itr->status())) {
                // found subdirectory,
                // create tagger
                auto sax = itr->path();
                string name = sax.filename().native();
                if (name.find(pat) != 0) {
                    continue;
                }
                paths.push_back(sax);
            }
        }
        CHECK(paths.size() == 1);
        return root/paths[0];
    }

    cv::Mat virtical_extend (cv::Mat in, unsigned r) {
        if (r == 0) return in;
        cv::Mat extended;
        vconcat3(in.rowRange(in.rows - r, in.rows),
                 in,
                 in.rowRange(0, r),
                 &extended);
        return extended;
    }

    cv::Mat virtical_unextend (cv::Mat in, int r) {
        if (r == 0) return in;
        return in.rowRange(r, in.rows - r);
    }

    void ApplyDetector (string const &name,
                        Slice *slice,
                        int FROM, int TO,
                        float scale, unsigned vext) {
        cv::Mat from = slice->images[FROM];
        if (!from.data) return;
        from = virtical_extend(from, vext);
        cv::Mat to;
        Detector *det = Detector::get(name);
        CHECK(det) << " cannot create detector.";
        det->apply(from, &to);
        slice->images[TO] = virtical_unextend(to, vext);
        CHECK(slice->images[TO].isContinuous());
        if (scale != 1.0) {
            slice->images[TO] *= scale;
        }
    }

    void ApplyDetector (string const &name,
                        Study *study,
                        int FROM, int TO,
                        float scale, unsigned vext) {
        //string bound_model = config.get("adsb2.caffe.bound_model", (home_dir/fs::path("bound_model")).native());
        vector<Slice *> slices;
        study->pool(&slices);
        //config.put("adsb2.caffe.model", "model2");
        std::cerr << "Applying model " << name << " to "  << slices.size() << "  slices..." << std::endl;
        boost::progress_display progress(slices.size(), std::cerr);
//#define CPU_ONLY 1
#ifdef CPU_ONLY
#pragma omp parallel for schedule(dynamic, 1)
        for (unsigned i = 0; i < slices.size(); ++i) {
            cv::Mat from = slices[i]->images[FROM];
            if (!from.data) continue;
            from = virtical_extend(from, vext);
            cv::Mat to;
            Detector *det = Detector::get(name);
            CHECK(det) << " cannot create detector.";
            det->apply(from, &to);
            slices[i]->images[TO] = virtical_unextend(to, vext);
            CHECK(slices[i]->images[TO].isContinuous());
            if (scale != 1.0) {
                slices[i]->images[TO] *= scale;
            }
#pragma omp critical
            ++progress;
        }
#else   // batch processing using GPU
        Detector *det = Detector::get(name);
        CHECK(det) << " cannot create detector.";
        unsigned i = 0;
        while (i < slices.size()) {
            vector<cv::Mat> input;
            vector<cv::Mat *> output;
            int seen = 0;
            while ((i < slices.size()) && (input.size() < caffe_batch)) {
                cv::Mat from = slices[i]->images[FROM];
                if (from.data) {
                    input.push_back(virtical_extend(from, vext));
                    output.push_back(&slices[i]->images[TO]);
                }
                ++seen;
                ++i;
            }
            vector<cv::Mat> tmp;
            det->apply(input, &tmp);
            CHECK(tmp.size() == input.size());
            for (unsigned j = 0; j < input.size(); ++j) {
                *output[j] = virtical_unextend(tmp[j], vext);
                CHECK(output[j]->isContinuous());
                if (scale != 1.0) {
                    *output[j] *= scale;
                }
            }
            progress += seen;
        }
#endif
    }

    cv::Point_<float> weighted_box_center (cv::Mat &prob, cv::Rect box) {
        cv::Mat roi = prob(box);
        float sx = 0;
        float sy = 0;
        float s = 0;
        for (int y = 0; y < roi.rows; ++y) {
            float const *ptr = roi.ptr<float const>(y);
            for (int x = 0; x < roi.cols; ++x) {
                float w = ptr[x];
                sx += x * w;
                sy += y * w;
                s += w;
            }
        }
        return cv::Point_<float>(box.x + sx/s, box.y + sy/s);
    }

    static int path_to_study_id (fs::path const &p) {
        fs::path last;
        for (auto c: p) {
            if (c.native() == "study") {
                break;
            }
            last = c;
        }
        return lexical_cast<int>(last.native());
    }

    void SliceReport::reprobe_meta (fs::path const &root) {
        fs::path newp = path;
        if (!root.empty()) {
            vector<fs::path> comps(path.begin(), path.end());
            newp = root;
            for (unsigned i = 1; i < comps.size(); ++i) {
                if (comps[i].native() == "study") {
                    for (unsigned j = i-1; j < comps.size(); ++j) {
                        newp /= comps[j];
                    }
                }
            }
        }
        load_dicom(newp, &meta);
    }

    void SliceReport::parse (string const &line) {
        istringstream ss(line);
        float area;
        ss >> path;
        ss >> sax_id >> slice_id;
        ss >> area;
        ss >> box.x;
        ss >> box.y;
        ss >> box.width;
        ss >> box.height;
        ss >> polar_box.x;
        ss >> polar_box.y;
        ss >> polar_box.width;
        ss >> polar_box.height;
        ss >> meta.slice_location >> meta.trigger_time >> meta.spacing >> meta.raw_spacing;
        study_id = path_to_study_id(path);
        for (auto &v: meta) {
            ss >> v;
        }
        for (auto &v: data) {
            ss >> v;
        }
        data[SL_AREA] = area;
        //CHECK(ss);
        CHECK(area == data[SL_AREA]);
    }

    StudyReport::StudyReport (fs::path const &path) {
        fs::ifstream is(path);
        if (!is) return;
        string line;
        vector<SliceReport> all;
        int max_slice = 0;
        int max_sax = 0;
        while (getline(is, line)) {
            SliceReport s;
            s.parse(line);
            if (s.slice_id > max_slice) max_slice = s.slice_id;
            if (s.sax_id > max_sax) max_sax = s.sax_id;
            if (all.size()) {
                CHECK(all.back().study_id == s.study_id);
            }
            all.push_back(s);
        }
        ++max_slice;
        ++max_sax;
        resize(max_sax);
        for (auto &s: all) {
            at(s.sax_id).push_back(s);
        }
        /*
        for (auto const &ss: *this) {
            CHECK(ss.size() == max_slice);
        }
        */
    }

    StudyReport::StudyReport (Study const &sss) {
        resize(sss.size());
        for (unsigned i = 0; i < sss.size(); ++i) {
            auto const &from = sss[i];
            auto &to = at(i);
            to.resize(from.size());
            for (unsigned j = 0; j < from.size(); ++j) {
                auto const &slice = from[j];
                auto &rep = to[j];
                rep.sax_id = i;
                rep.slice_id = j;
                rep.path = slice.path;
                rep.box = slice.box;
                rep.polar_box = slice.polar_box;
                rep.meta = slice.meta;
                rep.data = slice.data;
            }
        }
    }

    void StudyReport::dump (std::ostream &os) {
        for (auto const &ss: *this) {
            for (auto const &s: ss) {
                os << s.path.native()
                    << '\t' << s.sax_id
                    << '\t' << s.slice_id
                    << '\t' << s.data[SL_AREA]
                    << '\t' << s.box.x
                    << '\t' << s.box.y
                    << '\t' << s.box.width
                    << '\t' << s.box.height
                    << '\t' << s.polar_box.x
                    << '\t' << s.polar_box.y
                    << '\t' << s.polar_box.width
                    << '\t' << s.polar_box.height
                    << '\t' << s.meta.slice_location
                    << '\t' << s.meta.trigger_time
                    << '\t' << s.meta.spacing
                    << '\t' << s.meta.raw_spacing;
                for (auto const &v: s.meta) {
                    os << '\t' << v;
                }
                for (auto const &v: s.data) {
                    os << '\t' << v;
                }
                os << std::endl;
            }
        }
    }

    void NaiveGaussianAcc (float v, float scale, vector<float> *s) {
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

    void GaussianAcc::apply (float v, float scale,
                             vector<float> *ps) const {
        NaiveGaussianAcc(v, scale, ps);
#if 0
        if (extend < 0) {
            return;
        }
        if (v < 1) v = 1;
        if (v >= Eval::VALUES) v = Eval::VALUES-1;
        if (scale < 1) scale = 1;

        vector<float> s(Eval::VALUES);
        for (int i = 0; i < Eval::VALUES; ++i) {
            s[i] =  //0.5 * (1 + erf(float(i - v) / scale /sqrt(2)));
                    0.5 * erfc(-(i-v)/scale/M_SQRT2);
        }
        if (extend > 1) {
            LOG(WARNING) << "touchup gaussian acc";
            float minl = s[0];
            int lv = floor(v);
            CHECK(lv > 0);
            int rv = lv + 1;
            // s[0]: minl => 0
            // s[lv]: same
            for (int i = 0; i < lv; ++i) {
                s[i] -= minl * (lv - i) / lv;
                if (s[i] < 0) s[i] = 0;
            }

            float gapR = 1.0 - s.back();
            for (int i = rv; i < Eval::VALUES; ++i) {
                s[i] += gapR * (1 + i - rv) / (Eval::VALUES - rv);
                if (s[i] > 1) s[i] = 1;
                
            }
        }
        for (unsigned i = 0; i < s.size(); ++i) {
            if (s[i] < 0) {
                LOG(ERROR) << fmt::format("s[{}] < 0: {}", i, s[i]);
                s[i] = 0;
            }
            if (s[i] > 1) {
                LOG(ERROR) << fmt::format("s[{}] > 1: {}", i, s[i]);
                s[i] = 1;
            }
            if (i > 0) {
                if (s[i] < s[i-1]) {
                    LOG(ERROR) << fmt::format("s[{}] < s[{}]", i, i-1);
                    s[i] = s[i-1];
                }
            }
        }

        ps->swap(s);
#endif
        /*
        float lgap = v;
        float lscale = scale;
        if (lgap < lscale * extend) {
            lscale = lgap / extend;
        }
        float rgap = 600 - v;
        float rscale = scale;
        if (rgap < rscale * extend) {
            rscale = rgap / extend;
        }

        int int_v = floor(v);
        s->resize(Eval::VALUES);
        float sum = 0;
        for (unsigned i = 0; i <= int_v; ++i) {
            float x = (float(i) - v) / lscale;
            x = exp(-0.5 * x * x);
            s->at(i) = x;
            sum += x;
        }
        for (unsigned i = int_v + 1; i <= Eval::VALUES; ++i) {
            float x = (float(i) - v) / rscale;
            x = exp(-0.5 * x * x);
            s->at(i) = x;
            sum += x;
        }
        float acc = 0;
        for (auto &v: *s) {
            acc += v;
            v = acc / sum;
        }
        */
    }

    bool Sampler::polar (cv::Mat from_image,
                      cv::Mat from_label,
                      cv::Mat *to_image,
                      cv::Mat *to_label,
                      bool) {
        cv::Rect box;
        cv::Mat shrink;
        cv::erode(from_label, shrink, polar_kernel);
        bound_box<uint8_t>(from_label, &box);
        CHECK(box.width > 0);
        CHECK(box.height > 0);
        // randomization
        float color = 0;
        cv::Point_<float> C(box.x + box.width/2, box.y + box.height/2);
        float R = std::min(box.width, box.height)/2;
        bool flip = false;
        {
            float dx, dy, dr;
#pragma omp critical
            {
                float cr = polar_C(e) * R;  // center perturb
                float phi = polar_phi(e);
                flip = ((e() % 2) == 1);
                color = delta_color(e);
                dr = polar_R(e);
                dx = cr * std::cos(phi);
                dy = cr * std::sin(phi);
            }
            cv::Point p(std::round(C.x + dx), std::round(C.y + dy));
            uint8_t v = shrink.at<uint8_t>(p);
            if (v == 0) return false;
                // otherwise we found a center out side of circle, retry
            C.x += dx;
            C.y += dy;
            R = max_R(C, box) * dr;
        }
        linearPolar(from_image, to_image, C, R, CV_INTER_LINEAR+CV_WARP_FILL_OUTLIERS);
        linearPolar(from_label, to_label, C, R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS);
        /*
        imageF.convertTo(image, CV_8UC1);
        cv::equalizeHist(image, image);
        labelF.convertTo(label, CV_8UC1);
        */
        *to_image += color;
        cv::morphologyEx(*to_label, *to_label, cv::MORPH_CLOSE, polar_kernel);
        if (flip) {
            cv::flip(*to_image, *to_image, 0);
            cv::flip(*to_label, *to_label, 0);
        }
        return true;
    }

    //unsigned Eval::CASES = 500;

char const *VERSION = ADSB2_VERSION;
char const *HEADER = "Id,P0,P1,P2,P3,P4,P5,P6,P7,P8,P9,P10,P11,P12,P13,P14,P15,P16,P17,P18,P19,P20,P21,P22,P23,P24,P25,P26,P27,P28,P29,P30,P31,P32,P33,P34,P35,P36,P37,P38,P39,P40,P41,P42,P43,P44,P45,P46,P47,P48,P49,P50,P51,P52,P53,P54,P55,P56,P57,P58,P59,P60,P61,P62,P63,P64,P65,P66,P67,P68,P69,P70,P71,P72,P73,P74,P75,P76,P77,P78,P79,P80,P81,P82,P83,P84,P85,P86,P87,P88,P89,P90,P91,P92,P93,P94,P95,P96,P97,P98,P99,P100,P101,P102,P103,P104,P105,P106,P107,P108,P109,P110,P111,P112,P113,P114,P115,P116,P117,P118,P119,P120,P121,P122,P123,P124,P125,P126,P127,P128,P129,P130,P131,P132,P133,P134,P135,P136,P137,P138,P139,P140,P141,P142,P143,P144,P145,P146,P147,P148,P149,P150,P151,P152,P153,P154,P155,P156,P157,P158,P159,P160,P161,P162,P163,P164,P165,P166,P167,P168,P169,P170,P171,P172,P173,P174,P175,P176,P177,P178,P179,P180,P181,P182,P183,P184,P185,P186,P187,P188,P189,P190,P191,P192,P193,P194,P195,P196,P197,P198,P199,P200,P201,P202,P203,P204,P205,P206,P207,P208,P209,P210,P211,P212,P213,P214,P215,P216,P217,P218,P219,P220,P221,P222,P223,P224,P225,P226,P227,P228,P229,P230,P231,P232,P233,P234,P235,P236,P237,P238,P239,P240,P241,P242,P243,P244,P245,P246,P247,P248,P249,P250,P251,P252,P253,P254,P255,P256,P257,P258,P259,P260,P261,P262,P263,P264,P265,P266,P267,P268,P269,P270,P271,P272,P273,P274,P275,P276,P277,P278,P279,P280,P281,P282,P283,P284,P285,P286,P287,P288,P289,P290,P291,P292,P293,P294,P295,P296,P297,P298,P299,P300,P301,P302,P303,P304,P305,P306,P307,P308,P309,P310,P311,P312,P313,P314,P315,P316,P317,P318,P319,P320,P321,P322,P323,P324,P325,P326,P327,P328,P329,P330,P331,P332,P333,P334,P335,P336,P337,P338,P339,P340,P341,P342,P343,P344,P345,P346,P347,P348,P349,P350,P351,P352,P353,P354,P355,P356,P357,P358,P359,P360,P361,P362,P363,P364,P365,P366,P367,P368,P369,P370,P371,P372,P373,P374,P375,P376,P377,P378,P379,P380,P381,P382,P383,P384,P385,P386,P387,P388,P389,P390,P391,P392,P393,P394,P395,P396,P397,P398,P399,P400,P401,P402,P403,P404,P405,P406,P407,P408,P409,P410,P411,P412,P413,P414,P415,P416,P417,P418,P419,P420,P421,P422,P423,P424,P425,P426,P427,P428,P429,P430,P431,P432,P433,P434,P435,P436,P437,P438,P439,P440,P441,P442,P443,P444,P445,P446,P447,P448,P449,P450,P451,P452,P453,P454,P455,P456,P457,P458,P459,P460,P461,P462,P463,P464,P465,P466,P467,P468,P469,P470,P471,P472,P473,P474,P475,P476,P477,P478,P479,P480,P481,P482,P483,P484,P485,P486,P487,P488,P489,P490,P491,P492,P493,P494,P495,P496,P497,P498,P499,P500,P501,P502,P503,P504,P505,P506,P507,P508,P509,P510,P511,P512,P513,P514,P515,P516,P517,P518,P519,P520,P521,P522,P523,P524,P525,P526,P527,P528,P529,P530,P531,P532,P533,P534,P535,P536,P537,P538,P539,P540,P541,P542,P543,P544,P545,P546,P547,P548,P549,P550,P551,P552,P553,P554,P555,P556,P557,P558,P559,P560,P561,P562,P563,P564,P565,P566,P567,P568,P569,P570,P571,P572,P573,P574,P575,P576,P577,P578,P579,P580,P581,P582,P583,P584,P585,P586,P587,P588,P589,P590,P591,P592,P593,P594,P595,P596,P597,P598,P599";
}

