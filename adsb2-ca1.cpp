#include <limits>
#include <boost/multi_array.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include "adsb2.h"

namespace adsb2 {
    static inline double distance (cv::Point2f const &p1, cv::Point2f const &p2) {
        double x = p1.x - p2.x;
        double y = p1.y - p2.y;
        return sqrt(x * x + y * y);
    }

    struct WorkSpaceEntry {
        float pixel[2];      // pixel[0]: color
                             // pixel[1]: probability
                             
        cv::Point2f pt;      // cartesion coordinate

        float opt;           // optimal value
        int prev;            // pointer to prev row's optimal column
        cv::Point2f prev0;   // optimal location of 0, for circular trick
    };

    typedef boost::multi_array<WorkSpaceEntry, 2> WorkSpaceBase;

    class WorkSpace:public WorkSpaceBase {
    public:
        WorkSpace (cv::Mat image, cv::Mat prob, float polar_R): WorkSpaceBase(boost::extents[image.rows][image.cols]) {
            CHECK(image.cols == prob.cols);
            CHECK(image.rows == prob.rows);
            int rows = image.rows;
            int cols = image.cols;
            for (int y = 0; y < rows; ++y) {
                float const *p_i = image.ptr<float const>(y);
                float const *p_p = prob.ptr<float const>(y);
                auto *e = &((*this)[y][0]);
                double phi = M_PI * 2 * y / image.rows;
                for (int x = 0; x < cols; ++x) {
                    double rho = x * polar_R / image.cols;
                    cv::Point2f pt(rho * std::cos(phi), rho * std::sin(phi));
                    e[x].pt = pt; //slice->polar_C + pt;
                    e[x].pixel[0] = p_i[x];
                    e[x].pixel[1] = p_p[x];
                    e[x].opt = -std::numeric_limits<float>::max();
                    e[x].prev = -1;
                }
            }
        }

        void run (vector<std::pair<int, int>> const &range,
                  vector<int> *seg,
                  unsigned key,
                  bool mono,
                  vector<float> th,
                  float nd,
                  float smooth,
                  int max_gap,
                  float scost) {
            size_t rows = shape()[0];
            size_t cols = shape()[1];
            CHECK(th.size() == rows);
            int best_cc = 0;
            for (int y = 0; y < rows; ++y) {
                auto *e = &((*this)[y][0]);
                if (y == 0) {   // first row, no connection to previous
                    float acc = 0;
                    float last_delta = std::numeric_limits<float>::max();
                    for (int x = range[y].first; x < range[y].second; ++x) {
                        float delta = e[x].pixel[key] - th[y];
                        if (delta < 0) delta *= nd;
                        if (mono) {
                            if (delta > last_delta) {
                                delta = last_delta;
                            }
                            else {
                                last_delta = delta;
                            }
                        }
                        acc += delta - scost;
                        e[x].opt = acc;
                        e[x].prev = -1;
                    }
                    continue;
                }
                auto *prev = &((*this)[y-1][0]);
                float acc = 0;
                best_cc = -1;
                float best_cc_score = -1;
                float last_delta = std::numeric_limits<float>::max();
                for (int x = range[y].first; x < range[y].second; ++x) {
                    float delta = e[x].pixel[key] - th[y];
                    if (delta < 0) delta *= nd;
                    if (mono) {
                        if (delta > last_delta) {
                            delta = last_delta;
                        }
                        else {
                            last_delta = delta;
                        }
                    }
                    acc += delta - scost;
                    int lb = std::max(x - max_gap, range[y-1].first);
                    int ub = std::min(x + max_gap + 1, range[y-1].second);
                    float best_score = -std::numeric_limits<float>::max();
                    int best_prev = 0;
                    for (int p = lb; p < ub; ++p) {
                        float score = prev[p].opt + acc - smooth * distance(prev[p].pt, e[x].pt);
                        if (y + 1 == rows) {  // need to consider connection to 0
                            score -= smooth * distance(prev[p].prev0, e[x].pt);
                        }
                        CHECK((score >= best_score) || (score <= best_score));
                        if (score > best_score) {
                            best_score = score;
                            best_prev = p;
                        }
                    }
                    e[x].opt = best_score;
                    e[x].prev = best_prev;
                    if (y == 1) {
                        e[x].prev0 = prev[e[x].prev].pt;
                    }
                    else {
                        e[x].prev0 = prev[best_prev].prev0;
                    }
                    if ((best_cc < 0) || (best_score > best_cc_score)) {
                        best_cc_score = best_score;
                        best_cc = x;
                    }
                }
            }
            int y = rows - 1;
            seg->clear();
            while (y >= 0) {
                seg->push_back(best_cc);
                best_cc = (*this)[y][best_cc].prev;
                --y;
            }
#if 0       // test polar transformation code
#pragma omp critical
            do {
                static int cc = 0;
                ++cc;
                if (cc < 150) break;
                cv::Mat image = slice->images[IM_IMAGE];
                vector<cv::Point> pts;
                for (unsigned i = 0; i < seg.size(); ++i) {
                    pts.push_back(ws[i][seg[i]].pt);
                }
                cv::Point const *ppts = &pts[0];
                int npts = seg.size();
                cv::fillPoly(image, &ppts, &npts, 1, cv::Scalar(0xFF));
                cv::imwrite("xxx.png", image);
                exit(1);
            } while (false);
#endif
            CHECK(best_cc = -1);
            std::reverse(seg->begin(), seg->end());
        }
    };

    class CA1: public CA {
        float smooth1;
        float smooth2;
        int margin1;
        int margin2;
        int gap;
        float thr1;
        float thr2;
        int extra_delta;
        int extra_minus;
        float extra_th;
        bool do_extend;
        float ndisc;
        float wctrpct;
        float bctrpct;
        int mink;
        int W;
        float scost2;
        bool gth2;
#if 0
        float penalty (float dx) const {
            float v = smooth * dx;
            /*
#pragma omp critical
            std::cerr << dx << '\t' << v << std::endl;
            */
            return v;
        };
#endif
        void get_dp1_th (cv::Mat image, vector<float> *ths) const {
            float th = 0;
            {
                float big_mean = cv::mean(image.colRange(0, margin1))[0];
                float small_mean = cv::mean(image.colRange(image.cols - margin1, image.cols))[0];
                //cerr << left_mean << ' ' << right_mean << endl;
                if (!(small_mean < big_mean)) {
                    th = std::max(small_mean, big_mean);
                }
                else {
                    th = small_mean + (big_mean - small_mean) * thr1;
                }
            }
            ths->resize(image.rows);
            for (auto &v: *ths) {
                v = th;
            }
        }

        float contour_avg (cv::Mat image, vector<int> const &ctr, int delta, float pct, int sign, float *sigma = nullptr) const {
            CHECK(ctr.size() == image.rows);
            vector<float> v;
            // extract color along the contour
            for (unsigned i = 0; i < ctr.size(); ++i) {
                float const *ptr = image.ptr<float const>(i);
                int x = ctr[i] + delta;
                if (x < 0) x = 0;
                if (x >= image.cols) x = image.cols - 1;
                v.push_back(ptr[x]);
            }
            CHECK(v.size() == ctr.size());

            int N = ctr.size() * pct;
            if (N > ctr.size()) N = ctr.size();
            CHECK(N > ctr.size() / 2);

            int best_begin = 0;

            if (N < ctr.size()) {
                // replicate once
                for (unsigned i = 0; i < ctr.size(); ++i) {
                    v.push_back(v[i]);
                }
                v.push_back(v.front()); // and add head again
                vector<float> intv(v.size());
                std::partial_sum(v.begin(), v.end(), intv.begin());
                float best_diff = std::numeric_limits<float>::max() * sign;
                for (unsigned i = 0; i + N < intv.size(); ++i) {
                    float diff = (intv[i+N] - intv[i]) * sign;
                    if (diff < best_diff) {
                        best_begin = i;
                        best_diff = diff;
                    }
                }
            }

            namespace ba = boost::accumulators;
            typedef ba::accumulator_set<double, ba::stats<ba::tag::mean, ba::tag::variance>> Acc;
            Acc acc;
            for (unsigned i = 0; i < N; ++i) {
                acc(v[best_begin + i]);
            }
            if (sigma) {
                *sigma = std::sqrt(ba::variance(acc));
            }
            return ba::mean(acc);
        }

        void get_dp2_th (cv::Mat image, vector<int> const &ctr, int lbb, int bound, vector<float> *ths) const {
            float big_mean = cv::mean(image.colRange(0, margin1))[0];
            ths->resize(image.rows);
            if (gth2) {
                float th = 0;
                {
                    float small_mean = big_mean;
                    for (int i = -lbb; i < bound; ++i) {
                        float x = contour_avg(image, ctr, i, bctrpct, 1);
                        if (x < small_mean) small_mean = x;
                    }
                    th = small_mean + (big_mean - small_mean) * thr2;
                }
                std::fill(ths->begin(), ths->end(), th);
                return; 
            }
            cv::Mat kernel = cv::Mat::ones(mink, mink, CV_8U);
            cv::Mat eroded;
            cv::erode(image, eroded, kernel);
            for (int y = 0; y < image.rows; ++y) {
                float const *ptr = eroded.ptr<float const>(y);
                float th = 0;
                /*
                float small_mean = cv::mean(image.colRange(image.cols - margin, image.cols))[0];
                //cerr << left_mean << ' ' << right_mean << endl;
                if (!(small_mean < big_mean)) return std::max(small_mean, big_mean);
                th = small_mean + (big_mean - small_mean) * thr;
                */
                float small = big_mean;
                for (int i = -lbb; i < bound; ++i) {
                    int x = ctr[y] + i;
                    if (x < 0) x = 0;
                    if (x >= image.cols) x = image.cols - 1;
                    if (ptr[x] < small) small = ptr[x];
                }
                th = small + (big_mean - small) * thr2;
                ths->at(y) = th;
            }
        }

        // return absolute bound
        void find_shift (cv::Mat image, vector<int> const &ctr, int *bound, int *xx) const {
            int L1 = margin1;   // 5
            int L2 = margin2;   // 40
            vector<float> wavg(L1 + L2 +1);       // array index i <-> delta     i - L1
            vector<float> bavg(L1 + L2 +1);       // array index i <-> delta     i - L1
                                                 //             0                -L1
                                                 //             L1                 0
                                                 //             L1 + L2            L2
            vector<float> grad(bavg.size(), 0);   // actually reverse gradient
            vector<float> sigma(bavg.size());
            for (int i = -L1; i <= L2; ++i) {
                float s;
                wavg[L1+i] = contour_avg(image, ctr, i, wctrpct, -1);
                bavg[L1+i] = contour_avg(image, ctr, i, bctrpct, 1, &s);
                sigma[L1+i] = s;
            }
            // compute delta
            int P1 = 0;
            for (int i = W; i + W < grad.size(); ++i) {
                grad[i] = wavg[i-W] - bavg[i+W];
                if (grad[i] > grad[P1]) {
                    P1 = i;
                }
            }
            *xx = P1 + W - L1;
            // P1 is roughly a transition point from white to black
            int P2 = std::max(P1, L1);
            for (int i = P2; i < sigma.size(); ++i) {
                if (sigma[i] < sigma[P2]) {
                    P2 = i;
                }
            }
            float max_sigma = sigma[P2] + extra_th;
            for (; P2 + 1 < sigma.size(); ++P2) {
                if (sigma[P2 + 1] > max_sigma) break;
            }
            // P2 is the point with deepest black
            //
            CHECK(P2 < sigma.size());

            /*
            float best = -std::numeric_limits<float>::max();
            int best_nleft = 0;
            float left = 0;

            float sum = 0;
            for (unsigned i = 0; i <= P2; ++i) {
                sum += bavg[i];
            }

            for (unsigned nleft = 1; nleft <= P2; ++nleft) {
                left += bavg[nleft-1];
                float right = sum - left;
                float nright = P2 + 1 - nleft;

                float gap = left / nleft - right / nright;
                if (gap > best) {
                    best = gap;
                    best_nleft = nleft;
                }
            }
            *delta = best_nleft - L1 + extra_delta;
            if (*delta < 0) *delta = 0;
            */
            *bound = P2 + 1 - L1 + extra_delta;
            /*
            if (*delta > *bound - 1) {
                *delta = *bound - 1;
            }
            */
        }

        void helper (Slice *slice, vector<int> *plb = nullptr, int *pbound = nullptr) const {
            cv::Mat image = slice->images[IM_POLAR];
            cv::Mat prob = slice->images[IM_POLAR_PROB];
            int rows = image.rows;
            int cols = image.cols;
            WorkSpace ws(image, prob, slice->polar_R);
            vector<int> contour;
            {
                vector<std::pair<int, int>> range1;
                {
                    for (int i = 0; i < image.rows; ++i) {
                        range1.push_back(std::make_pair(0, cols));
                    }
                }
                vector<float> th;
                get_dp1_th(prob, &th);
                ws.run(range1, &contour, 1, false, th, 1.0, smooth1, gap, 0);
            }
            int bound, lbb;
            find_shift(image, contour, &bound, &lbb);
            {
                cv::Mat xv(image.size(), CV_32F, cv::Scalar(0));
                for (int y = 0; y < xv.rows; ++y) {
                    float *row = xv.ptr<float>(y);
                    int lb = std::max(0, contour[y]);
                    int ub = std::min(contour[y] + bound, xv.cols);
                    for (int x = lb; x < ub; ++x) {
                        row[x] = 1;
                    }
                }
                cv::Mat xc;
                linearPolar(xv, &xc, slice->polar_C, slice->polar_R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS+CV_WARP_INVERSE_MAP);
                slice->data[SL_XA] = cv::sum(xc)[0];
            }
            if (do_extend) {
                // extend 1
                if (plb) *plb = contour;
                if (pbound) *pbound = bound;
                vector<std::pair<int, int>> range2(rows);
                for (int i = 0; i < rows; ++i) {
                    range2[i].first = std::max(0, contour[i] - extra_minus);
                    range2[i].second = std::min(contour[i] + bound, cols);
                }
                vector<float> th;
                get_dp2_th(image, contour, extra_minus, bound, &th);
                ws.run(range2, &contour, 0, true, th, ndisc, smooth2, gap, scost2);
            }
            slice->polar_contour.swap(contour);
        }

    public:
        CA1 (Config const &conf)
            : margin1(conf.get<int>("adsb2.ca1.margin1", 5)),
            margin2(conf.get<int>("adsb2.ca1.margin2", 30)),
            thr1(conf.get<float>("adsb2.ca1.th1", 0.7)),
            thr2(conf.get<float>("adsb2.ca1.th2", 0.04)),
            smooth1(conf.get<float>("adsb2.ca1.smooth1", 10)),
            smooth2(conf.get<float>("adsb2.ca1.smooth2", 30)),
            extra_delta(conf.get<int>("adsb2.ca1.extra", 0)),
            extra_minus(conf.get<int>("adsb2.ca1.minus", 0)),
            extra_th(conf.get<float>("adsb2.ca1.eth", 0)),
            gap(conf.get<int>("adsb2.ca1.gap", 7)),
            do_extend(conf.get<int>("adsb2.ca1.extend", 1) > 0),
            ndisc(conf.get<float>("adsb2.ca1.ndisc", 0.4)),
            wctrpct(conf.get<float>("adsb2.ca1.wctrpct", 0.9)),
            bctrpct(conf.get<float>("adsb2.ca1.ctrpct", 0.8)),
            mink(conf.get<int>("adsb2.ca1.mink", 3)),
            W(conf.get<int>("adsb2.ca1.W", 2)),
            scost2(conf.get<float>("adsb2.ca1.scost2", 0)),
            gth2(conf.get<int>("adsb2.ca1.gth2", 0) != 0)
        {
        }
        void apply_slice (Slice *s, vector<int> *plb, int *pbound) {
            helper(s, plb, pbound);
        }
        void apply_slice (Slice *s) {
            helper(s);
        }
        void apply (Series *ss) const {
#pragma omp parallel for
            for (unsigned i = 0; i < ss->size(); ++i) {
                helper(&ss->at(i));
            }
        }
    };

    void study_CA1 (Slice *pslice, Config const &config, bool vis) {
        CA1 ca1(config);
        Slice &slice = *pslice;
        if (!slice.images[IM_POLAR_PROB].data) {
            slice.polar_box = cv::Rect();
            return;
        }
        vector<int> lb;
        int bound;
        ca1.apply_slice(&slice, &lb, &bound);
        if (slice.polar_contour.empty()) {
            slice.data[SL_AREA] = 0;
            return;
        }
        auto const &cc = slice.polar_contour;
        CHECK(cc.size() == slice.images[IM_IMAGE].rows);
        cv::Mat polar(slice.images[IM_IMAGE].size(), CV_32F, cv::Scalar(0));
        for (int y = 0; y < polar.rows; ++y) {
            float *row = polar.ptr<float>(y);
            //for (int x = 0; x <= cc[y]; ++x) {
            for (int x = 0; x < cc[y]; ++x) {
                row[x] = 1;
            }
        }

        linearPolar(polar, &slice.images[IM_LABEL], slice.polar_C, slice.polar_R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS+CV_WARP_INVERSE_MAP);

        bound_box(slice.images[IM_LABEL], &slice.polar_box);

        {
            static int constexpr ext = 5;
            cv::Rect box = slice.polar_box;
            box.x -= ext;
            box.y -= ext;
            box.width += ext * 2;
            box.height += ext * 2;
            box = box & cv::Rect(cv::Point(0,0), slice.images[IM_IMAGE].size());

            cv::Mat inside;
            linearPolar(polar, &inside, slice.polar_C, slice.polar_R, CV_INTER_NN+CV_WARP_FILL_OUTLIERS+CV_WARP_INVERSE_MAP);
            cv::Mat mask = inside(box).clone();
            type_convert(&mask, CV_8U);
            cv::Mat color = slice.images[IM_IMAGE](box).clone();
            float cs1, ps1; // inside
            color_sum(color, mask, &cs1, &ps1);
            CHECK(cs1 >= 0);
            if (ps1 <= 0) {
                LOG(ERROR) << "ps1 == 0: " << ps1;
                ps1 = 1;
            }
            CHECK(ps1 > 0);

            cv::Mat kernel = cv::Mat::ones(ext, ext, CV_8U);
            cv::dilate(mask, mask, kernel);
            float cs2, ps2; // outside
            color_sum(color, mask, &cs2, &ps2);
            cs2 -= cs1;
            ps2 -= ps1;
            CHECK(cs2 >= 0);
            if (ps2 <= 0) {
                LOG(ERROR) << "ps2 <= 0: " << ps2;
                ps2 = 1;
            }
            CHECK(ps2 > 0);

            cs1 /= ps1;
            cs2 /= ps2;
            slice.data[SL_CCOLOR] = cs1 - cs2;
        }
        

        slice.data[SL_AREA] = cv::sum(slice.images[IM_LABEL])[0];
        slice.data[SL_PSCORE] = box_score(slice.images[IM_LABEL], slice.polar_box);
        slice.data[SL_CSCORE] = box_score(slice.images[IM_LABEL], slice.box);

        if (vis) {
            cv::Mat vis_cart;
            cv::Mat vis(slice.images[IM_IMAGE].size(), CV_32F, cv::Scalar(0));
            for (int i = 1; i < cc.size(); ++i) {
                cv::line(vis, cv::Point(cc[i-1], i-1), cv::Point(cc[i], i), cv::Scalar(-0xFF), 2);
            }
            linearPolar(vis, &vis_cart, slice.polar_C, slice.polar_R, CV_INTER_LINEAR+CV_WARP_FILL_OUTLIERS + CV_WARP_INVERSE_MAP);
            if (lb.size()) { 
                for (int i = 1; i < cc.size(); ++i) {
                        cv::line(vis, cv::Point(lb[i-1], i-1), cv::Point(lb[i], i), cv::Scalar(-0xFF), 1);
                        cv::line(vis, cv::Point(lb[i-1] + bound, i-1), cv::Point(lb[i] + bound, i), cv::Scalar(-0xFF), 1);
                }
            }
            hconcat3(slice.images[IM_POLAR] + vis, slice.images[IM_POLAR_PROB] * 255 + vis, slice.images[IM_IMAGE] + vis_cart, &slice._extra);
        }
    }

    void study_CA1 (Study *study, Config const &config, bool vis) {
        // compute bouding box
        vector<Slice *> tasks;
        study->pool(&tasks);
        CA1 ca1(config);
#pragma omp parallel for schedule(dynamic, 1)
        for (unsigned i = 0; i < tasks.size(); ++i) {
            study_CA1(tasks[i], config, vis);
        }
    }

}
