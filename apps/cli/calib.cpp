// apps/cli/calib.cpp — key = value 标定文件解析

#include "calib.h"

#include "stereo/stereo_c_api.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cli {
namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// 读一串 float，数量必须等于 want。
bool parse_floats(const std::string& text, float* out, int want) {
    std::istringstream ss(text);
    int n = 0;
    float v = 0;
    while (ss >> v) {
        if (n >= want) return false;
        out[n++] = v;
    }
    return n == want;
}

bool parse_int(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_float(const std::string& text, float& out) {
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

float param_default(int id) {
    const stereo_param_desc* d = stereo_param_find(id);
    return d != nullptr ? d->default_value : 0.0f;
}

}  // namespace

bool load_calibration(const std::string& path, Calibration& out, std::string& err) {
    // 先取参数表默认值（唯一真源），文件只覆盖出现的键。
    out.max_disp = static_cast<int>(param_default(STEREO_PARAM_MAX_DISP));
    out.p1 = static_cast<int>(param_default(STEREO_PARAM_P1));
    out.p2 = static_cast<int>(param_default(STEREO_PARAM_P2));
    out.uniqueness = param_default(STEREO_PARAM_UNIQUENESS);
    out.lr_check = static_cast<int>(param_default(STEREO_PARAM_LR_CHECK));
    out.min_z = param_default(STEREO_PARAM_MIN_Z);
    out.max_z = param_default(STEREO_PARAM_MAX_Z);

    std::ifstream in(path);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = path + ":" + std::to_string(lineno) + ": expected 'key = value'";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        bool ok = true;
        if (key == "fx") ok = parse_float(val, out.fx);
        else if (key == "fy") ok = parse_float(val, out.fy);
        else if (key == "cx") ok = parse_float(val, out.cx);
        else if (key == "cy") ok = parse_float(val, out.cy);
        else if (key == "ufx") ok = parse_float(val, out.ufx);
        else if (key == "ufy") ok = parse_float(val, out.ufy);
        else if (key == "ucx") ok = parse_float(val, out.ucx);
        else if (key == "ucy") ok = parse_float(val, out.ucy);
        else if (key == "R") ok = parse_floats(val, out.R, 9);
        else if (key == "t") ok = parse_floats(val, out.t, 3);
        else if (key == "src_width") ok = parse_int(val, out.src_width);
        else if (key == "work_width") ok = parse_int(val, out.work_width);
        else if (key == "work_height") ok = parse_int(val, out.work_height);
        else if (key == "flip") ok = parse_int(val, out.flip);
        else if (key == "dy") ok = parse_float(val, out.dy);
        else if (key == "max_disp") ok = parse_int(val, out.max_disp);
        else if (key == "p1") ok = parse_int(val, out.p1);
        else if (key == "p2") ok = parse_int(val, out.p2);
        else if (key == "uniqueness") ok = parse_float(val, out.uniqueness);
        else if (key == "lr_check") ok = parse_int(val, out.lr_check);
        else if (key == "min_z") ok = parse_float(val, out.min_z);
        else if (key == "max_z") ok = parse_float(val, out.max_z);
        else {
            err = path + ":" + std::to_string(lineno) + ": unknown key '" + key + "'";
            return false;
        }
        if (!ok) {
            err = path + ":" + std::to_string(lineno) + ": bad value for '" + key + "'";
            return false;
        }
    }

    // 超广角内参缺省 = 宽相机（对角双摄的真实值应从设备读，CLI 只接受文件）。
    if (out.ufx == 0.0f) { out.ufx = out.fx; out.ufy = out.fy; out.ucx = out.cx; out.ucy = out.cy; }
    if (out.fx <= 0 || out.fy <= 0) {
        err = "calibration: fx/fy must be positive";
        return false;
    }
    if (out.src_width <= 0) {
        err = "calibration: src_width is required";
        return false;
    }
    if (out.work_width <= 0 || out.work_height <= 0) {
        err = "calibration: work_width/work_height must be positive";
        return false;
    }
    return true;
}

}  // namespace cli
