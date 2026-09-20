#include "face_db_web/enroll_policy.hpp"

#include "face_db_web/json.hpp"

#include <fstream>
#include <sstream>

namespace face_db_web {
namespace {

std::optional<bool> readBool(const json::Value& v, const char* key) {
    const json::Value* f = v.find(key);
    if (!f || f->type != json::Value::Type::Bool) return std::nullopt;
    return f->boolean;
}

std::optional<float> readFloat(const json::Value& v, const char* key) {
    const json::Value* f = v.find(key);
    if (!f || f->type != json::Value::Type::Number) return std::nullopt;
    return static_cast<float>(f->number);
}

}  // namespace

bool loadEnrollPolicy(const std::string& path, EnrollPolicy& out, std::string& error) {
    if (path.empty()) return true;  // nothing requested: keep defaults

    std::ifstream file(path);
    if (!file) {
        error = "policy file not readable: " + path;
        return false;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    // Parser only views its input: the string must outlive it. Binding
    // oss.str() directly would leave a dangling string_view.
    const std::string text = oss.str();

    json::Parser parser(text);
    json::Value root;
    if (!parser.parse(root) || !root.isObject()) {
        error = "policy file is not a JSON object: " + parser.error();
        return false;
    }

    if (auto v = readFloat(root, "high_similarity")) out.high_similarity = *v;
    if (auto v = readFloat(root, "high_similarity_threshold")) out.high_similarity = *v;
    if (auto v = readBool(root, "require_confirmation")) out.require_confirmation = *v;
    if (auto v = readBool(root, "confirm_on_same_scene")) out.confirm_on_same_scene = *v;
    if (auto v = readBool(root, "confirm_on_cross_scene")) out.confirm_on_cross_scene = *v;
    if (auto v = readBool(root, "allow_replace")) out.allow_replace = *v;

    const json::Value* rules = root.find("scene_rules");
    if (rules && rules->isObject()) {
        for (const auto& kv : rules->object) {
            SceneRule rule;
            rule.threshold              = readFloat(kv.second, "threshold");
            rule.confirm_required       = readBool(kv.second, "confirm_required");
            rule.allow_auto_cross_scene = readBool(kv.second, "allow_auto_cross_scene");
            rule.allow_replace          = readBool(kv.second, "allow_replace");
            out.scene_rules[std::string(kv.first)] = rule;
        }
    }

    if (out.high_similarity <= 0.0f || out.high_similarity >= 1.0f) {
        error = "high_similarity must be in (0,1)";
        return false;
    }
    return true;
}

std::string defaultPolicyJson() {
    return R"({
  // 相似度 >= 该值即视为"可能是同一个人"，进入人工确认环节
  "high_similarity": 0.80,

  // 总开关：false = 关闭人工确认（不再拦截任何入库，不推荐）
  "require_confirmation": true,

  // 库中匹配记录与新图"同场景"时：是否必须人工确认（默认必须）
  "confirm_on_same_scene": true,

  // 库中匹配记录与新图"跨场景"时：是否必须人工确认
  // 设为 false 即"非同一场景直接入库、不提示"
  "confirm_on_cross_scene": true,

  // 是否允许"替换库中记录"这种处理方式
  "allow_replace": true,

  // 按场景配置策略（键为 scene 名）；缺省字段回落到上面的全局值
  "scene_rules": {
    "office": {
      "threshold": 0.80,
      "confirm_required": true,
      "allow_auto_cross_scene": false,
      "allow_replace": true
    },
    "visitor": {
      "threshold": 0.85,
      "confirm_required": false,
      "allow_auto_cross_scene": true
    }
  }
}
)";
}

}  // namespace face_db_web
