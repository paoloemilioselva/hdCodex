#include "hdcodex/materialx/materialx_compiler.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hdcodex {
namespace {

constexpr float kEpsilon = 1.0e-5F;

struct Value {
    enum class Kind { Numeric, Text, Texture, Geometric } kind{Kind::Numeric};
    std::string type;
    std::array<float, 4> number{};
    std::string text;
    std::string texture;
    std::string textureColorSpace;
    int textureChannel{-1};
    bool textureInverted{false};
    float normalScale{1.0F};
};

const MaterialXProgramInput* FindInput(
    const MaterialXProgramNode& node, std::string_view name)
{
    const auto found = std::ranges::find_if(node.inputs, [name](const auto& input) {
        return input.name == name;
    });
    return found == node.inputs.end() ? nullptr : &*found;
}

std::size_t ComponentCount(std::string_view type)
{
    if (type == "color4" || type == "vector4") return 4U;
    if (type == "color3" || type == "vector3") return 3U;
    if (type == "vector2") return 2U;
    return 1U;
}

Value ParseValue(std::string_view type, std::string_view source)
{
    Value result;
    result.type = std::string(type);
    if (type == "string" || type == "filename") {
        result.kind = Value::Kind::Text;
        result.text = std::string(source);
        return result;
    }
    if (type == "boolean") {
        result.number[0] = source == "true" || source == "1" ? 1.0F : 0.0F;
        return result;
    }

    std::string normalized(source);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream stream(normalized);
    const std::size_t count = ComponentCount(type);
    for (std::size_t component = 0; component < count; ++component) {
        if (!(stream >> result.number[component])) {
            if (component == 0U) result.number[0] = 0.0F;
            else result.number[component] = result.number[component - 1U];
        }
    }
    if (count == 1U) {
        result.number[1] = result.number[2] = result.number[3] = result.number[0];
    }
    return result;
}

bool Near(float left, float right)
{
    return std::abs(left - right) <= kEpsilon;
}

bool NumericIs(const Value& value, float expected)
{
    if (value.kind != Value::Kind::Numeric) return false;
    const std::size_t count = ComponentCount(value.type);
    for (std::size_t component = 0; component < count; ++component) {
        if (!Near(value.number[component], expected)) return false;
    }
    return true;
}

std::string NodeError(const MaterialXProgramNode& node, std::string_view message)
{
    return "MaterialX closure node '" + node.name + "' (" + node.nodeDef +
        ") " + std::string(message);
}

class ProgramEvaluator final {
public:
    explicit ProgramEvaluator(
        const MaterialXGeneratedProgram& program,
        const MaterialXEvaluationContext* context = nullptr)
        : _context(context)
    {
        for (const MaterialXProgramNode& node : program.nodes) {
            _nodes.emplace(node.name, &node);
        }
    }

    const MaterialXProgramNode& Node(std::string_view name) const
    {
        const auto found = _nodes.find(name);
        if (found == _nodes.end()) {
            throw std::runtime_error("MaterialX closure references missing node '" +
                std::string(name) + "'");
        }
        return *found->second;
    }

    const MaterialXProgramNode& Upstream(
        const MaterialXProgramNode& node, std::string_view inputName) const
    {
        const MaterialXProgramInput* input = FindInput(node, inputName);
        if (!input || input->upstreamNode.empty()) {
            throw std::runtime_error(NodeError(
                node, "has no connected '" + std::string(inputName) + "' input"));
        }
        return Node(input->upstreamNode);
    }

    Value Input(const MaterialXProgramNode& node, std::string_view name) const
    {
        const MaterialXProgramInput* input = FindInput(node, name);
        if (!input) {
            throw std::runtime_error(NodeError(
                node, "is missing required input '" + std::string(name) + "'"));
        }
        if (input->upstreamNode.empty()) return ParseValue(input->type, input->value);
        return Evaluate(input->upstreamNode, input->upstreamOutput);
    }

    Value InputOr(const MaterialXProgramNode& node, std::string_view name,
                  std::string_view type, std::string_view value) const
    {
        return FindInput(node, name) ? Input(node, name) : ParseValue(type, value);
    }

    Value Evaluate(std::string_view name, std::string_view output = {}) const
    {
        const std::string key = std::string(name) + "." + std::string(output);
        if (const auto cached = _cache.find(key); cached != _cache.end()) {
            return cached->second;
        }
        if (!_active.insert(key).second) {
            throw std::runtime_error("cycle in expanded MaterialX closure program at '" +
                std::string(name) + "'");
        }
        Value result = EvaluateNode(Node(name), output);
        if (result.kind == Value::Kind::Numeric &&
            ComponentCount(result.type) == 1U) {
            result.number[1] = result.number[2] = result.number[3] = result.number[0];
        }
        _active.erase(key);
        _cache.emplace(key, result);
        return result;
    }

    Value Roughness(const MaterialXProgramNode& closure) const
    {
        const MaterialXProgramInput* input = FindInput(closure, "roughness");
        if (!input) return ParseValue("float", "0");
        if (input->upstreamNode.empty()) return ParseValue(input->type, input->value);
        return Evaluate(input->upstreamNode, input->upstreamOutput);
    }

    Value NormalTexture(const MaterialXProgramNode& closure) const
    {
        const MaterialXProgramInput* input = FindInput(closure, "normal");
        if (!input || input->upstreamNode.empty()) return {};
        const MaterialXProgramNode& normal = Node(input->upstreamNode);
        if (normal.category == "normal") {
            Value none;
            none.kind = Value::Kind::Geometric;
            return none;
        }
        if (normal.category == "normalmap") {
            const Value scale = InputOr(normal, "scale", "float", "1");
            if (scale.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    normal, "uses a texture-driven normal-map scale"));
            }
            Value mapped;
            try {
                mapped = FindInput(normal, "in")
                    ? Input(normal, "in") : Input(normal, "normal");
            } catch (const std::runtime_error&) {
                Value none;
                none.kind = Value::Kind::Geometric;
                return none;
            }
            if (mapped.kind != Value::Kind::Texture) {
                throw std::runtime_error(NodeError(
                    normal, "does not resolve to one image texture"));
            }
            mapped.normalScale = scale.number[0];
            return mapped;
        }
        Value none;
        none.kind = Value::Kind::Geometric;
        return none;
    }

private:
    Value EvaluateNode(
        const MaterialXProgramNode& node, std::string_view output) const
    {
        if (node.category == "image") {
            const MaterialXProgramInput* fileInput = FindInput(node, "file");
            const Value file = fileInput
                ? Input(node, "file") : ParseValue("filename", "");
            if (file.kind != Value::Kind::Text) {
                throw std::runtime_error(NodeError(node, "has a non-filename file input"));
            }
            if (file.text.empty()) {
                if (FindInput(node, "default")) return Input(node, "default");
                Value fallback = ParseValue(node.type, "0");
                fallback.type = node.type;
                return fallback;
            }
            Value result;
            result.type = node.type;
            result.texture = file.text;
            result.textureColorSpace = fileInput ? fileInput->colorSpace : std::string();
            if (_context && _context->sampleTexture) {
                std::array<float, 2> texcoord = _context->texcoord;
                if (FindInput(node, "texcoord")) {
                    const Value coordinates = Input(node, "texcoord");
                    if (coordinates.kind != Value::Kind::Numeric ||
                        ComponentCount(coordinates.type) < 2U) {
                        throw std::runtime_error(NodeError(
                            node, "has a non-numeric texcoord input"));
                    }
                    texcoord = {
                        coordinates.number[0], coordinates.number[1]};
                }
                result.kind = Value::Kind::Numeric;
                const std::array<float, 4> sampled = _context->sampleTexture(
                    result.texture, result.textureColorSpace, texcoord);
                result.number = sampled;
            } else {
                result.kind = Value::Kind::Texture;
            }
            return result;
        }
        if (node.category == "geompropvalue" || node.category == "normal" ||
            node.category == "tangent" || node.category == "bitangent" ||
            node.category == "position" || node.category == "texcoord") {
            Value result;
            result.type = node.type;
            if (!_context) {
                result.kind = Value::Kind::Geometric;
                return result;
            }
            result.kind = Value::Kind::Numeric;
            if (node.category == "geompropvalue" || node.category == "texcoord") {
                result.number = {
                    _context->texcoord[0], _context->texcoord[1], 0.0F, 0.0F};
            } else if (node.category == "position") {
                result.number = {
                    _context->position[0], _context->position[1],
                    _context->position[2], 0.0F};
            } else {
                const std::array<float, 3>* direction = &_context->normal;
                if (node.category == "tangent") direction = &_context->tangent;
                else if (node.category == "bitangent") {
                    direction = &_context->bitangent;
                }
                result.number = {
                    (*direction)[0], (*direction)[1], (*direction)[2], 0.0F};
            }
            return result;
        }
        if (node.category == "artistic_ior") {
            const Value reflectivity = Input(node, "reflectivity");
            const Value edgeColor = Input(node, "edge_color");
            if (reflectivity.kind != Value::Kind::Numeric ||
                edgeColor.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has texture-driven complex IOR unsupported by this ABI"));
            }
            Value ior = reflectivity;
            Value extinction = reflectivity;
            ior.type = extinction.type = "color3";
            for (std::size_t component = 0; component < 3U; ++component) {
                const float r = std::clamp(reflectivity.number[component], 0.0F, 0.99F);
                const float root = std::sqrt(r);
                const float minimum = (1.0F - r) / (1.0F + r);
                const float maximum = (1.0F + root) / std::max(1.0F - root, kEpsilon);
                const float eta = maximum * (1.0F - edgeColor.number[component]) +
                    minimum * edgeColor.number[component];
                const float plus = eta + 1.0F;
                const float minus = eta - 1.0F;
                const float k2 = std::max(
                    (plus * plus * r - minus * minus) / std::max(1.0F - r, kEpsilon),
                    0.0F);
                ior.number[component] = eta;
                extinction.number[component] = std::sqrt(k2);
            }
            return output == "extinction" ? extinction : ior;
        }
        if (node.category == "roughness_anisotropy" ||
            node.category == "glossiness_anisotropy") {
            Value roughness = node.category == "roughness_anisotropy"
                ? Input(node, "roughness") : Input(node, "glossiness");
            const Value anisotropy = InputOr(node, "anisotropy", "float", "0");
            if (roughness.kind == Value::Kind::Texture) {
                roughness.type = "vector2";
                return roughness;
            }
            if (roughness.kind != Value::Kind::Numeric ||
                anisotropy.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has dynamic anisotropic roughness"));
            }
            float value = roughness.number[0];
            if (node.category == "glossiness_anisotropy") value = 1.0F - value;
            const float alpha = std::clamp(value * value, kEpsilon, 1.0F);
            const float amount = std::clamp(anisotropy.number[0], 0.0F, 0.98F);
            const float aspect = std::sqrt(1.0F - amount);
            Value result = roughness;
            result.type = "vector2";
            result.number = amount > 0.0F
                ? std::array<float, 4>{
                    std::min(alpha / aspect, 1.0F), alpha * aspect, 0.0F, 0.0F}
                : std::array<float, 4>{alpha, alpha, 0.0F, 0.0F};
            return result;
        }
        if (node.category == "roughness_dual") {
            Value result = Input(node, "roughness");
            if (result.kind == Value::Kind::Texture) return result;
            if (result.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has dynamic dual roughness"));
            }
            if (result.number[1] < 0.0F) result.number[1] = result.number[0];
            result.number[0] = std::clamp(
                result.number[0] * result.number[0], kEpsilon, 1.0F);
            result.number[1] = std::clamp(
                result.number[1] * result.number[1], kEpsilon, 1.0F);
            result.type = "vector2";
            return result;
        }

        if (node.category == "convert") {
            Value result = Input(node, "in");
            result.type = node.type;
            if (result.kind == Value::Kind::Numeric) {
                const std::size_t count = ComponentCount(node.type);
                if (count > 1U && ComponentCount(FindInput(node, "in")->type) == 1U) {
                    for (std::size_t component = 1; component < count; ++component) {
                        result.number[component] = result.number[0];
                    }
                }
            }
            return result;
        }
        if (node.category == "extract") {
            Value result = Input(node, "in");
            const Value index = InputOr(node, "index", "integer", "0");
            const int component = static_cast<int>(index.number[0]);
            if (component < 0 || component > 3) {
                throw std::runtime_error(NodeError(node, "has an invalid extract index"));
            }
            result.number[0] = result.number[static_cast<std::size_t>(component)];
            result.type = node.type;
            result.textureChannel = component;
            return result;
        }
        if (node.category == "invert") {
            Value result = Input(node, "in");
            if (result.kind == Value::Kind::Texture) {
                result.textureInverted = !result.textureInverted;
                result.type = node.type;
                return result;
            }
            if (result.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has a non-numeric invert input"));
            }
            const std::size_t count = ComponentCount(node.type);
            for (std::size_t component = 0; component < count; ++component) {
                result.number[component] = 1.0F - result.number[component];
            }
            result.type = node.type;
            return result;
        }
        if (node.category == "combine2") {
            const Value first = Input(node, "in1");
            const Value second = Input(node, "in2");
            if (first.kind == Value::Kind::Numeric &&
                second.kind == Value::Kind::Numeric) {
                Value result = first;
                result.type = node.type;
                result.number = {first.number[0], second.number[0], 0.0F, 0.0F};
                return result;
            }
            if (first.kind == Value::Kind::Texture &&
                second.kind == Value::Kind::Texture &&
                first.texture == second.texture &&
                first.textureChannel == 0 && second.textureChannel == 1 &&
                !first.textureInverted && !second.textureInverted) {
                Value result = first;
                result.type = node.type;
                result.textureChannel = -1;
                return result;
            }
            // The compact ABI retains the source map when a generated
            // roughness graph performs per-axis arithmetic on that map.
            if (first.kind == Value::Kind::Texture) return first;
            if (second.kind == Value::Kind::Texture) return second;
            throw std::runtime_error(NodeError(
                node, "does not combine numeric or same-image channels"));
        }
        if (node.category == "combine3") {
            const Value red = Input(node, "in1");
            const Value green = Input(node, "in2");
            const Value blue = Input(node, "in3");
            if (red.kind == Value::Kind::Texture &&
                green.kind == Value::Kind::Texture &&
                blue.kind == Value::Kind::Texture &&
                red.texture == green.texture && red.texture == blue.texture &&
                red.textureChannel == 0 && green.textureChannel == 1 &&
                blue.textureChannel == 2 && !red.textureInverted &&
                !blue.textureInverted) {
                Value result = red;
                result.type = node.type;
                result.textureChannel = -1;
                // Preserve the only non-identity operation in the common
                // MaterialX normal-map channel reconstruction.
                result.textureInverted = green.textureInverted;
                return result;
            }
            if (red.kind == Value::Kind::Numeric &&
                green.kind == Value::Kind::Numeric &&
                blue.kind == Value::Kind::Numeric) {
                Value result = red;
                result.type = node.type;
                result.number = {red.number[0], green.number[0], blue.number[0], 0.0F};
                return result;
            }
            throw std::runtime_error(NodeError(
                node, "does not combine three channels from one image"));
        }
        if (node.category == "luminance") {
            const Value input = Input(node, "in");
            if (input.kind == Value::Kind::Texture) return input;
            if (input.kind != Value::Kind::Numeric) return input;
            Value result = input;
            const Value coefficients = InputOr(
                node, "lumacoeffs", "color3", "0.2722287,0.6740818,0.0536895");
            const float luminance = input.number[0] * coefficients.number[0] +
                input.number[1] * coefficients.number[1] +
                input.number[2] * coefficients.number[2];
            result.number = {luminance, luminance, luminance, luminance};
            result.type = node.type;
            return result;
        }
        if (node.category == "ifgreater" || node.category == "ifgreatereq" ||
            node.category == "ifless" || node.category == "iflesseq" ||
            node.category == "ifequal" || node.category == "ifnotequal") {
            const Value left = Input(node, "value1");
            const Value right = Input(node, "value2");
            if (left.kind != Value::Kind::Numeric || right.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has a dynamic comparison"));
            }
            bool condition = false;
            if (node.category == "ifgreater") condition = left.number[0] > right.number[0];
            else if (node.category == "ifgreatereq") condition = left.number[0] >= right.number[0];
            else if (node.category == "ifless") condition = left.number[0] < right.number[0];
            else if (node.category == "iflesseq") condition = left.number[0] <= right.number[0];
            else if (node.category == "ifequal") condition = Near(left.number[0], right.number[0]);
            else condition = !Near(left.number[0], right.number[0]);
            return Input(node, condition ? "in1" : "in2");
        }
        if (node.category == "clamp") {
            Value input = Input(node, "in");
            const Value low = InputOr(node, "low", node.type, "0");
            const Value high = InputOr(node, "high", node.type, "1");
            if (input.kind == Value::Kind::Texture ||
                input.kind == Value::Kind::Geometric) {
                return input;
            }
            if (input.kind != Value::Kind::Numeric || low.kind != Value::Kind::Numeric ||
                high.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has non-numeric clamp inputs"));
            }
            const std::size_t count = ComponentCount(node.type);
            for (std::size_t component = 0; component < count; ++component) {
                input.number[component] = std::clamp(
                    input.number[component], low.number[component], high.number[component]);
            }
            input.type = node.type;
            return input;
        }
        if (node.category == "mix") {
            const Value amount = Input(node, "mix");
            if (amount.kind != Value::Kind::Numeric) {
                const Value background = Input(node, "bg");
                if (background.kind == Value::Kind::Texture) return background;
                const Value foreground = Input(node, "fg");
                if (foreground.kind == Value::Kind::Texture) return foreground;
                return background;
            }
            if (Near(amount.number[0], 0.0F)) return Input(node, "bg");
            if (Near(amount.number[0], 1.0F)) return Input(node, "fg");
            const Value foreground = Input(node, "fg");
            const Value background = Input(node, "bg");
            if (foreground.kind == Value::Kind::Texture) return foreground;
            if (background.kind == Value::Kind::Texture) return background;
            if (foreground.kind != Value::Kind::Numeric ||
                background.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "mixes texture values"));
            }
            Value result = background;
            const std::size_t count = ComponentCount(node.type);
            for (std::size_t component = 0; component < count; ++component) {
                result.number[component] = foreground.number[component] * amount.number[0] +
                    background.number[component] * (1.0F - amount.number[0]);
            }
            result.type = node.type;
            return result;
        }

        const auto binary = [&](auto operation, bool textureIdentity) {
            const Value left = Input(node, "in1");
            const Value right = Input(node, "in2");
            if (left.kind == Value::Kind::Texture || right.kind == Value::Kind::Texture) {
                if (textureIdentity) {
                    if (left.kind == Value::Kind::Texture && NumericIs(right, 1.0F)) {
                        return left;
                    }
                    if (right.kind == Value::Kind::Texture && NumericIs(left, 1.0F)) {
                        return right;
                    }
                }
                if (left.kind == Value::Kind::Texture) return left;
                if (right.kind == Value::Kind::Texture) return right;
            }
            if (left.kind != Value::Kind::Numeric || right.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has non-numeric operands"));
            }
            Value result = left;
            result.type = node.type;
            const std::size_t count = ComponentCount(node.type);
            for (std::size_t component = 0; component < count; ++component) {
                result.number[component] = operation(
                    left.number[component], right.number[component]);
            }
            return result;
        };

        if (node.category == "add") return binary(std::plus<float>{}, false);
        if (node.category == "subtract") return binary(std::minus<float>{}, false);
        if (node.category == "multiply") return binary(std::multiplies<float>{}, true);
        if (node.category == "divide") {
            return binary([](float left, float right) {
                return left / right;
            }, false);
        }
        if (node.category == "modulo") {
            return binary([](float left, float right) {
                return std::abs(right) > kEpsilon
                    ? std::fmod(left, right) : 0.0F;
            }, false);
        }
        if (node.category == "power") {
            const Value base = Input(node, "in1");
            const Value exponent = Input(node, "in2");
            if (base.kind == Value::Kind::Texture && NumericIs(exponent, 1.0F)) {
                return base;
            }
            if (base.kind == Value::Kind::Texture) return base;
            return binary([](float left, float right) {
                return std::pow(std::max(left, 0.0F), right);
            }, false);
        }
        if (node.category == "min" || node.category == "max") {
            const Value left = Input(node, "in1");
            const Value right = Input(node, "in2");
            if (left.kind == Value::Kind::Texture && NumericIs(right,
                    node.category == "max" ? 0.0F : 1.0F)) return left;
            if (right.kind == Value::Kind::Texture && NumericIs(left,
                    node.category == "max" ? 0.0F : 1.0F)) return right;
            return node.category == "min"
                ? binary([](float a, float b) { return std::min(a, b); }, false)
                : binary([](float a, float b) { return std::max(a, b); }, false);
        }
        if (node.category == "dotproduct") {
            const Value left = Input(node, "in1");
            const Value right = Input(node, "in2");
            if (left.kind != Value::Kind::Numeric ||
                right.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has non-constant dot-product operands"));
            }
            Value result = ParseValue("float", "0");
            const std::size_t count = std::min(
                ComponentCount(left.type), ComponentCount(right.type));
            for (std::size_t component = 0U; component < count; ++component) {
                result.number[0] +=
                    left.number[component] * right.number[component];
            }
            result.number[1] = result.number[2] = result.number[3] =
                result.number[0];
            return result;
        }
        if (node.category == "crossproduct") {
            const Value left = Input(node, "in1");
            const Value right = Input(node, "in2");
            if (left.kind != Value::Kind::Numeric ||
                right.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has non-constant cross-product operands"));
            }
            Value result;
            result.type = node.type;
            result.number[0] = left.number[1] * right.number[2] -
                left.number[2] * right.number[1];
            result.number[1] = left.number[2] * right.number[0] -
                left.number[0] * right.number[2];
            result.number[2] = left.number[0] * right.number[1] -
                left.number[1] * right.number[0];
            return result;
        }
        if (node.category == "magnitude") {
            const Value input = Input(node, "in");
            if (input.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has a non-constant magnitude operand"));
            }
            float squaredLength = 0.0F;
            for (std::size_t component = 0U;
                 component < ComponentCount(input.type); ++component) {
                squaredLength += input.number[component] * input.number[component];
            }
            return ParseValue("float", std::to_string(std::sqrt(squaredLength)));
        }
        if (node.category == "atan2") {
            const char* yName = FindInput(node, "iny") ? "iny" : "in1";
            const char* xName = FindInput(node, "inx") ? "inx" : "in2";
            const Value y = Input(node, yName);
            const Value x = Input(node, xName);
            if (y.kind != Value::Kind::Numeric || x.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "has non-constant atan2 operands"));
            }
            return ParseValue("float", std::to_string(
                std::atan2(y.number[0], x.number[0])));
        }
        if (node.category == "sqrt" || node.category == "inversesqrt" ||
            node.category == "sign" || node.category == "ln" ||
            node.category == "exp" || node.category == "absval" ||
            node.category == "floor" || node.category == "ceil" ||
            node.category == "round" || node.category == "normalize" ||
            node.category == "sin" || node.category == "cos" ||
            node.category == "tan" || node.category == "asin" ||
            node.category == "acos") {
            Value result = Input(node, "in");
            if (result.kind == Value::Kind::Geometric && node.category == "normalize") {
                return result;
            }
            if (result.kind == Value::Kind::Texture) return result;
            if (result.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has a dynamic unary operand"));
            }
            const std::size_t count = ComponentCount(node.type);
            if (node.category == "normalize") {
                float squaredLength = 0.0F;
                for (std::size_t component = 0U; component < count; ++component) {
                    squaredLength += result.number[component] *
                        result.number[component];
                }
                const float inverseLength = 1.0F /
                    std::max(std::sqrt(squaredLength), kEpsilon);
                for (std::size_t component = 0U; component < count; ++component) {
                    result.number[component] *= inverseLength;
                }
                result.type = node.type;
                return result;
            }
            for (std::size_t component = 0; component < count; ++component) {
                float& value = result.number[component];
                if (node.category == "sqrt") value = std::sqrt(std::max(value, 0.0F));
                else if (node.category == "inversesqrt") {
                    value = 1.0F / std::sqrt(std::max(value, kEpsilon));
                }
                else if (node.category == "sign") value = value < 0.0F ? -1.0F : 1.0F;
                else if (node.category == "ln") value = std::log(std::max(value, kEpsilon));
                else if (node.category == "exp") value = std::exp(value);
                else if (node.category == "absval") value = std::abs(value);
                else if (node.category == "floor") value = std::floor(value);
                else if (node.category == "ceil") value = std::ceil(value);
                else if (node.category == "round") value = std::round(value);
                else if (node.category == "sin") value = std::sin(value);
                else if (node.category == "cos") value = std::cos(value);
                else if (node.category == "tan") value = std::tan(value);
                else if (node.category == "asin") {
                    value = std::asin(std::clamp(value, -1.0F, 1.0F));
                } else if (node.category == "acos") {
                    value = std::acos(std::clamp(value, -1.0F, 1.0F));
                }
            }
            result.type = node.type;
            return result;
        }

        if (node.category == "colorcorrect" || node.category == "remap") {
            if (FindInput(node, "in")) return Input(node, "in");
            if (FindInput(node, "input")) return Input(node, "input");
        }
        if (node.category == "saturate") {
            Value result = Input(node, "in");
            if (result.kind == Value::Kind::Texture) return result;
            if (result.kind == Value::Kind::Numeric) {
                const std::size_t count = ComponentCount(node.type);
                for (std::size_t component = 0; component < count; ++component) {
                    result.number[component] = std::clamp(
                        result.number[component], 0.0F, 1.0F);
                }
                result.type = node.type;
                return result;
            }
        }
        if (node.category == "ramp4") {
            std::array<Value, 4> corners{
                Input(node, "valuetl"), Input(node, "valuetr"),
                Input(node, "valuebl"), Input(node, "valuebr")};
            if (std::ranges::all_of(corners, [](const Value& value) {
                    return value.kind == Value::Kind::Numeric;
                })) {
                Value result = corners.front();
                const std::size_t count = ComponentCount(node.type);
                for (std::size_t component = 0; component < count; ++component) {
                    result.number[component] = 0.25F * (
                        corners[0].number[component] + corners[1].number[component] +
                        corners[2].number[component] + corners[3].number[component]);
                }
                result.type = node.type;
                return result;
            }
        }

        throw std::runtime_error(NodeError(node, "is an unsupported active value operation"));
    }

    std::map<std::string, const MaterialXProgramNode*, std::less<>> _nodes;
    const MaterialXEvaluationContext* _context{};
    mutable std::map<std::string, Value, std::less<>> _cache;
    mutable std::set<std::string, std::less<>> _active;
};

enum ClosureBits : unsigned int {
    ClosureNone = 0U,
    ClosureDiffuse = 1U << 0U,
    ClosureConductor = 1U << 1U,
    ClosureReflection = 1U << 2U,
    ClosureTransmission = 1U << 3U,
    ClosureSubsurface = 1U << 4U,
    ClosureSheen = 1U << 5U,
    ClosureTranslucent = 1U << 6U,
};

struct ClosureCompiler final {
    explicit ClosureCompiler(const MaterialXGeneratedProgram& source)
        : program(source), evaluator(source) {}

    SceneMaterial Compile(std::string_view terminalNodeDef)
    {
        if (program.outputNode.empty()) {
            throw std::runtime_error("MaterialX closure program has no output node");
        }
        const MaterialXProgramNode& surface = evaluator.Node(program.outputNode);
        if (surface.nodeDef != "ND_surface" && surface.category != "surface") {
            throw std::runtime_error(NodeError(
                surface, "is not the supported MaterialX surface constructor"));
        }
        material.shaderNodeId = std::string(terminalNodeDef);
        material.materialXOutputNode = program.outputNode;

        if (const MaterialXProgramInput* opacity = FindInput(surface, "opacity")) {
            AssignScalar(evaluator.Input(surface, opacity->name),
                         material.opacity, material.opacityTexture, surface, "opacity");
        }
        if (const MaterialXProgramInput* bsdf = FindInput(surface, "bsdf");
            bsdf && !bsdf->upstreamNode.empty()) {
            const MaterialXProgramNode& bsdfNode =
                evaluator.Node(bsdf->upstreamNode);
            containsDiffuseClosure =
                (Classify(bsdfNode) & ClosureDiffuse) != 0U;
            VisitBsdf(bsdfNode, 0, false);
        }
        if (const MaterialXProgramInput* edf = FindInput(surface, "edf");
            edf && !edf->upstreamNode.empty()) {
            try {
                const Value emission = EvaluateEdf(evaluator.Node(edf->upstreamNode));
                AssignColor(emission, material.emission, material.emissionTexture,
                            surface, "emission");
                material.emissionWeight = 1.0F;
            } catch (const std::runtime_error&) {
                // An unsupported emission decoration must not erase the
                // independently supported surface BSDF.
            }
        }
        FinalizeDielectrics();
        return material;
    }

private:
    struct Dielectric {
        const MaterialXProgramNode* node{};
        int layerDepth{};
    };

    unsigned int Classify(const MaterialXProgramNode& node)
    {
        if (node.category == "oren_nayar_diffuse_bsdf" ||
            node.category == "burley_diffuse_bsdf") return ClosureDiffuse;
        if (node.category == "conductor_bsdf" ||
            node.category == "generalized_schlick_bsdf") return ClosureConductor;
        if (node.category == "dielectric_bsdf") {
            const Value mode = evaluator.InputOr(node, "scatter_mode", "string", "R");
            return mode.text == "T" ? ClosureTransmission : ClosureReflection;
        }
        if (node.category == "subsurface_bsdf") return ClosureSubsurface;
        if (node.category == "translucent_bsdf") return ClosureTranslucent;
        if (node.category == "sheen_bsdf") return ClosureSheen;
        if (node.category == "mix" || node.category == "layer") {
            unsigned int result = ClosureNone;
            for (const char* name : {"fg", "bg", "top", "base"}) {
                const MaterialXProgramInput* input = FindInput(node, name);
                if (input && !input->upstreamNode.empty() && input->type == "BSDF") {
                    result |= Classify(evaluator.Node(input->upstreamNode));
                }
            }
            return result;
        }
        if (node.category == "multiply" && node.type == "BSDF") {
            return Classify(evaluator.Upstream(node, "in1"));
        }
        return ClosureNone;
    }

    bool Inactive(const MaterialXProgramNode& node)
    {
        if (!FindInput(node, "weight")) return false;
        const Value weight = evaluator.Input(node, "weight");
        return NumericIs(weight, 0.0F);
    }

    void VisitBsdf(const MaterialXProgramNode& node, int layerDepth, bool topLayer)
    {
        if (node.category == "mix") {
            const Value amount = evaluator.Input(node, "mix");
            const MaterialXProgramNode& foreground = evaluator.Upstream(node, "fg");
            const MaterialXProgramNode& background = evaluator.Upstream(node, "bg");
            const unsigned int foregroundKind = Classify(foreground);
            const unsigned int backgroundKind = Classify(background);
            bool semanticMix = false;
            if ((foregroundKind & ClosureConductor) != 0U &&
                (backgroundKind & ClosureConductor) == 0U) {
                AssignScalar(amount, material.metalness, material.metalnessTexture,
                             node, "metalness mix");
                semanticMix = true;
            } else if ((foregroundKind & ClosureTransmission) != 0U &&
                       (backgroundKind & ClosureTransmission) == 0U) {
                AssignScalarConstant(
                    amount, material.transmission, node, "transmission mix");
                semanticMix = true;
            } else if ((foregroundKind & ClosureSubsurface) != 0U &&
                       (backgroundKind & ClosureDiffuse) != 0U) {
                AssignScalar(amount, material.subsurface,
                             material.subsurfaceTexture, node, "subsurface mix");
                semanticMix = true;
            } else if ((foregroundKind & ClosureTranslucent) != 0U &&
                       (backgroundKind & ClosureDiffuse) != 0U) {
                ApplyScalarWeight(amount, material.translucentWeight,
                                  material.translucentWeightTexture,
                                  node, "translucent mix");
                semanticMix = true;
            } else if ((foregroundKind & ClosureDiffuse) != 0U &&
                       (backgroundKind & ClosureTranslucent) != 0U) {
                if (amount.kind != Value::Kind::Numeric) {
                    throw std::runtime_error(NodeError(
                        node, "has a texture-driven inverse translucent mix"));
                }
                Value inverseAmount = amount;
                inverseAmount.number[0] = 1.0F - inverseAmount.number[0];
                ApplyScalarWeight(inverseAmount, material.translucentWeight,
                                  material.translucentWeightTexture,
                                  node, "inverse translucent mix");
                semanticMix = true;
            }
            if (amount.kind == Value::Kind::Numeric && Near(amount.number[0], 0.0F)) {
                VisitBsdf(background, layerDepth, topLayer);
                return;
            }
            if (amount.kind == Value::Kind::Numeric && Near(amount.number[0], 1.0F)) {
                VisitBsdf(foreground, layerDepth, topLayer);
                return;
            }
            if (!semanticMix && foregroundKind == backgroundKind) {
                // The selected implementations represent the same closure
                // class (for example thin-film on/off); visit both only when
                // the mix is genuinely dynamic.
            } else if (!semanticMix &&
                       ((foregroundKind &
                         (ClosureDiffuse | ClosureSubsurface)) != 0U) &&
                       ((backgroundKind &
                         (ClosureDiffuse | ClosureSubsurface)) != 0U)) {
                // Expanded PBR graphs use this closure switch to select their
                // thin-walled subsurface approximation. It remains distinct
                // from MaterialX translucent_bsdf diffuse transmission.
                material.thinWalled = true;
            } else if (!semanticMix) {
                throw std::runtime_error(NodeError(
                    node, "is an active closure mix not representable by the current ABI"));
            }
            VisitBsdf(foreground, layerDepth, topLayer);
            VisitBsdf(background, layerDepth, topLayer);
            return;
        }
        if (node.category == "layer") {
            const MaterialXProgramInput* top = FindInput(node, "top");
            const MaterialXProgramInput* base = FindInput(node, "base");
            if (top && !top->upstreamNode.empty()) {
                const MaterialXProgramNode& topNode = evaluator.Node(top->upstreamNode);
                VisitBsdf(topNode, layerDepth, true);
            }
            if (base && !base->upstreamNode.empty()) {
                const MaterialXProgramNode& baseNode = evaluator.Node(base->upstreamNode);
                if (base->type == "VDF") {
                    try {
                        ParseVdf(baseNode);
                    } catch (const std::runtime_error&) {
                        // Keep the supported boundary closure when a
                        // procedural volume exceeds the compact ABI.
                    }
                }
                else VisitBsdf(baseNode, layerDepth + 1, false);
            }
            return;
        }
        if (node.category == "multiply" && node.type == "BSDF") {
            VisitBsdf(evaluator.Upstream(node, "in1"), layerDepth, topLayer);
            Value tint;
            try {
                tint = evaluator.Input(node, "in2");
            } catch (const std::runtime_error&) {
                return;
            }
            if (tint.kind == Value::Kind::Numeric && !NumericIs(tint, 1.0F)) {
                const std::size_t count = ComponentCount(tint.type);
                const auto scale = [&](std::array<float, 3>& color,
                                       const std::string& texture) {
                    if (!texture.empty()) return;
                    for (std::size_t component = 0; component < 3U; ++component) {
                        color[component] *= tint.number[
                            count == 1U ? 0U : component];
                    }
                };
                scale(material.baseColor, material.baseColorTexture);
                scale(material.transmissionColor, material.transmissionTexture);
                scale(material.specularColor, material.specularColorTexture);
                scale(material.coatColor, material.coatColorTexture);
                scale(material.translucentColor,
                      material.translucentColorTexture);
                scale(material.subsurfaceColor, material.subsurfaceColorTexture);
            }
            // A texture-driven BSDF energy compensation term is not yet part
            // of the compact transport ABI. Retain the supported closure and
            // its source textures instead of deleting the whole material.
            return;
        }
        if (Inactive(node)) return;

        if (node.category == "oren_nayar_diffuse_bsdf" ||
            node.category == "burley_diffuse_bsdf") {
            AssignScalar(evaluator.InputOr(node, "weight", "float", "1"),
                         material.diffuseWeight,
                         material.diffuseWeightTexture,
                         node, "diffuse weight");
            AssignColor(evaluator.Input(node, "color"), material.baseColor,
                        material.baseColorTexture, node, "diffuse color");
            AssignScalar(evaluator.InputOr(node, "roughness", "float", "0"),
                         material.diffuseRoughness,
                         material.diffuseRoughnessTexture,
                         node, "diffuse roughness");
            material.diffuseModel = node.category == "burley_diffuse_bsdf"
                ? SceneMaterial::DiffuseModel::Burley
                : SceneMaterial::DiffuseModel::OrenNayar;
            if (node.category == "oren_nayar_diffuse_bsdf") {
                const Value compensation = evaluator.InputOr(
                    node, "energy_compensation", "boolean", "false");
                if (compensation.kind != Value::Kind::Numeric) {
                    throw std::runtime_error(NodeError(
                        node, "has dynamic energy compensation"));
                }
                if (NumericIs(compensation, 1.0F)) {
                    material.diffuseModel =
                        SceneMaterial::DiffuseModel::OrenNayarEnergyCompensated;
                } else if (!NumericIs(compensation, 0.0F)) {
                    throw std::runtime_error(NodeError(
                        node, "has invalid energy compensation"));
                }
            }
            CaptureNormal(node);
            return;
        }
        if (node.category == "conductor_bsdf" ||
            node.category == "generalized_schlick_bsdf") {
            Value reflectivity;
            if (node.category == "generalized_schlick_bsdf") {
                reflectivity = evaluator.Input(node, "color0");
            } else {
                const MaterialXProgramInput* iorInput = FindInput(node, "ior");
                if (!iorInput) {
                    throw std::runtime_error(NodeError(node, "has no generated IOR source"));
                }
                const MaterialXProgramNode* source = iorInput->upstreamNode.empty()
                    ? nullptr : &evaluator.Node(iorInput->upstreamNode);
                if (source && source->category == "artistic_ior") {
                    reflectivity = evaluator.Input(*source, "reflectivity");
                } else {
                    const Value eta = evaluator.Input(node, "ior");
                    const Value extinction = evaluator.Input(node, "extinction");
                    if (eta.kind != Value::Kind::Numeric ||
                        extinction.kind != Value::Kind::Numeric) {
                        throw std::runtime_error(NodeError(
                            node, "has texture-driven complex IOR unsupported by this ABI"));
                    }
                    reflectivity = eta;
                    reflectivity.type = "color3";
                    for (std::size_t component = 0; component < 3U; ++component) {
                        const float e = eta.number[component];
                        const float k = extinction.number[component];
                        reflectivity.number[component] =
                            ((e - 1.0F) * (e - 1.0F) + k * k) /
                            ((e + 1.0F) * (e + 1.0F) + k * k);
                    }
                }
            }
            AssignColor(reflectivity, material.baseColor,
                        material.baseColorTexture, node, "conductor reflectivity");
            AssignRoughness(evaluator.Roughness(node), material.roughness,
                            material.roughnessV, material.roughnessTexture,
                            node, "conductor roughness");
            CaptureNormal(node);
            return;
        }
        if (node.category == "dielectric_bsdf") {
            const Value mode = evaluator.InputOr(node, "scatter_mode", "string", "R");
            if (mode.kind != Value::Kind::Text) {
                throw std::runtime_error(NodeError(node, "has a dynamic scatter mode"));
            }
            if (mode.text == "T") {
                if (material.transmission <= 0.0F && material.transmissionTexture.empty()) {
                    material.transmission = 1.0F;
                }
                AssignColor(evaluator.InputOr(node, "tint", "color3", "1,1,1"),
                            material.transmissionColor, material.transmissionTexture,
                            node, "transmission tint");
                const Value roughness = evaluator.Roughness(node);
                AssignRoughness(roughness, material.roughness,
                                material.roughnessV,
                                material.roughnessTexture, node,
                                "transmission roughness");
                const Value ior = evaluator.InputOr(node, "ior", "float", "1.5");
                AssignScalarConstant(ior, material.indexOfRefraction, node, "transmission IOR");
                CaptureNormal(node);
            } else if (mode.text == "R") {
                dielectrics.push_back({&node, topLayer ? layerDepth : layerDepth + 1000});
            } else {
                throw std::runtime_error(NodeError(node, "has an unsupported scatter mode"));
            }
            return;
        }
        if (node.category == "subsurface_bsdf") {
            if (material.subsurface <= 0.0F && material.subsurfaceTexture.empty()) {
                material.subsurface = 1.0F;
            }
            AssignColor(evaluator.Input(node, "color"), material.subsurfaceColor,
                        material.subsurfaceColorTexture, node, "subsurface color");
            AssignColor(evaluator.Input(node, "radius"), material.subsurfaceRadius,
                        material.subsurfaceRadiusTexture, node, "subsurface radius");
            AssignScalarConstant(evaluator.InputOr(node, "anisotropy", "float", "0"),
                                 material.subsurfaceScatterAnisotropy,
                                 node, "subsurface anisotropy");
            CaptureNormal(node);
            return;
        }
        if (node.category == "translucent_bsdf") {
            // A standalone diffuse-transmission primitive has no reflective
            // diffuse lobe. A following diffuse visit restores that lobe for
            // the canonical MaterialX diffuse/translucent mix.
            if (!containsDiffuseClosure) {
                material.diffuseWeight = 0.0F;
                material.diffuseWeightTexture.clear();
            }
            ApplyScalarWeight(evaluator.InputOr(node, "weight", "float", "1"),
                              material.translucentWeight,
                              material.translucentWeightTexture,
                              node, "translucent weight");
            AssignColor(evaluator.Input(node, "color"), material.translucentColor,
                        material.translucentColorTexture, node, "translucent color");
            CaptureNormal(node);
            return;
        }
        if (node.category == "sheen_bsdf") {
            AssignScalar(evaluator.InputOr(node, "weight", "float", "1"),
                         material.sheen, material.sheenTexture,
                         node, "sheen weight");
            AssignColor(evaluator.InputOr(node, "color", "color3", "1,1,1"),
                        material.sheenColor, material.sheenColorTexture,
                        node, "sheen color");
            AssignScalar(evaluator.InputOr(node, "roughness", "float", "0.3"),
                         material.sheenRoughness,
                         material.sheenRoughnessTexture,
                         node, "sheen roughness");
            const Value mode = evaluator.InputOr(
                node, "mode", "string", "conty_kulla");
            if (mode.kind != Value::Kind::Text ||
                (mode.text != "conty_kulla" && mode.text != "zeltner")) {
                throw std::runtime_error(NodeError(
                    node, "has an unsupported sheen mode"));
            }
            material.sheenMode = mode.text == "zeltner" ? 1U : 0U;
            CaptureNormal(node);
            return;
        }
        throw std::runtime_error(NodeError(node, "is an unsupported active BSDF primitive"));
    }

    void FinalizeDielectrics()
    {
        if (dielectrics.empty()) return;
        std::ranges::sort(dielectrics, {}, &Dielectric::layerDepth);
        const Dielectric& base = dielectrics.back();
        AssignDielectric(*base.node, false);
        if (dielectrics.size() > 1U) {
            const Dielectric& coat = dielectrics.front();
            if (coat.node != base.node) AssignDielectric(*coat.node, true);
        }
    }

    void AssignDielectric(const MaterialXProgramNode& node, bool coat)
    {
        float& weight = coat ? material.coat : material.specularWeight;
        std::string& weightTexture = coat ? material.coatTexture : material.specularTexture;
        auto& color = coat ? material.coatColor : material.specularColor;
        std::string& colorTexture = coat
            ? material.coatColorTexture : material.specularColorTexture;
        float& roughness = coat ? material.coatRoughness : material.roughness;
        float& roughnessV = coat ? material.coatRoughnessV : material.roughnessV;
        std::string& roughnessTexture = coat
            ? material.coatRoughnessTexture : material.roughnessTexture;
        float& ior = coat ? material.coatIndexOfRefraction : material.indexOfRefraction;
        AssignScalar(evaluator.InputOr(node, "weight", "float", "1"),
                     weight, weightTexture, node, coat ? "coat weight" : "specular weight");
        AssignColor(evaluator.InputOr(node, "tint", "color3", "1,1,1"),
                    color, colorTexture, node, coat ? "coat tint" : "specular tint");
        AssignRoughness(evaluator.Roughness(node), roughness, roughnessV,
                        roughnessTexture, node,
                        coat ? "coat roughness" : "specular roughness");
        AssignScalarConstant(evaluator.InputOr(node, "ior", "float", "1.5"),
                             ior, node, coat ? "coat IOR" : "specular IOR");
        CaptureNormal(node);
    }

    void ParseVdf(const MaterialXProgramNode& node)
    {
        if (node.category != "anisotropic_vdf" && node.category != "absorption_vdf") {
            throw std::runtime_error(NodeError(node, "is an unsupported VDF primitive"));
        }
        const Value absorption = evaluator.InputOr(
            node, "absorption", "vector3", "0,0,0");
        const Value scattering = evaluator.InputOr(
            node, "scattering", "vector3", "0,0,0");
        if (absorption.kind != Value::Kind::Numeric ||
            scattering.kind != Value::Kind::Numeric) {
            throw std::runtime_error(NodeError(node, "has texture-driven volume coefficients"));
        }
        material.transmissionDepth = 1.0F;
        for (std::size_t component = 0; component < 3U; ++component) {
            material.transmissionColor[component] =
                std::exp(-std::max(absorption.number[component], 0.0F));
            material.transmissionScatter[component] =
                std::max(scattering.number[component], 0.0F);
        }
        AssignScalarConstant(evaluator.InputOr(node, "anisotropy", "float", "0"),
                             material.transmissionScatterAnisotropy,
                             node, "volume anisotropy");
    }

    Value EvaluateEdf(const MaterialXProgramNode& node)
    {
        if (node.category == "uniform_edf") return evaluator.Input(node, "color");
        if (node.category == "generalized_schlick_edf") {
            return EvaluateEdf(evaluator.Upstream(node, "base"));
        }
        if (node.category == "mix") {
            const Value amount = evaluator.Input(node, "mix");
            if (amount.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "has a dynamic EDF mix"));
            }
            if (Near(amount.number[0], 0.0F)) {
                return EvaluateEdf(evaluator.Upstream(node, "bg"));
            }
            if (Near(amount.number[0], 1.0F)) {
                return EvaluateEdf(evaluator.Upstream(node, "fg"));
            }
            const Value foreground = EvaluateEdf(evaluator.Upstream(node, "fg"));
            const Value background = EvaluateEdf(evaluator.Upstream(node, "bg"));
            if (foreground.kind != Value::Kind::Numeric ||
                background.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "partially mixes texture-driven EDFs"));
            }
            Value result = background;
            result.type = "color3";
            const float blend = std::clamp(amount.number[0], 0.0F, 1.0F);
            for (std::size_t component = 0U; component < 3U; ++component) {
                result.number[component] = std::lerp(
                    background.number[component],
                    foreground.number[component], blend);
            }
            return result;
        }
        if (node.category == "add" && node.type == "EDF") {
            Value left = EvaluateEdf(evaluator.Upstream(node, "in1"));
            const Value right = EvaluateEdf(evaluator.Upstream(node, "in2"));
            if (left.kind != Value::Kind::Numeric ||
                right.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(
                    node, "adds texture-driven EDFs"));
            }
            left.type = "color3";
            for (std::size_t component = 0U; component < 3U; ++component) {
                left.number[component] += right.number[component];
            }
            return left;
        }
        if (node.category == "multiply" && node.type == "EDF") {
            Value base = EvaluateEdf(evaluator.Upstream(node, "in1"));
            const Value tint = evaluator.Input(node, "in2");
            if (base.kind != Value::Kind::Numeric || tint.kind != Value::Kind::Numeric) {
                throw std::runtime_error(NodeError(node, "multiplies a texture EDF"));
            }
            const std::size_t tintComponents = ComponentCount(tint.type);
            for (std::size_t component = 0; component < 3U; ++component) {
                base.number[component] *= tint.number[
                    tintComponents == 1U ? 0U : component];
            }
            base.type = "color3";
            return base;
        }
        throw std::runtime_error(NodeError(node, "is an unsupported active EDF primitive"));
    }

    void CaptureNormal(const MaterialXProgramNode& node)
    {
        const Value normal = evaluator.NormalTexture(node);
        if (normal.kind != Value::Kind::Texture) return;
        if (!material.normalTexture.empty() && material.normalTexture != normal.texture) {
            throw std::runtime_error(NodeError(
                node, "uses a different normal map from another active lobe"));
        }
        material.normalTexture = normal.texture;
        material.normalTextureFlipY = normal.textureInverted;
        material.normalTextureScale = normal.normalScale;
    }

    static void AssignScalarConstant(
        const Value& source, float& target,
        const MaterialXProgramNode& node, std::string_view role)
    {
        if (source.kind != Value::Kind::Numeric) {
            throw std::runtime_error(NodeError(
                node, "has non-constant " + std::string(role)));
        }
        target = source.number[0];
    }

    static void AssignScalar(
        const Value& source, float& target, std::string& texture,
        const MaterialXProgramNode& node, std::string_view role)
    {
        if (source.kind == Value::Kind::Texture) {
            if (source.textureChannel > 0 || source.textureInverted) {
                throw std::runtime_error(NodeError(
                    node, "uses a packed or inverted scalar texture channel"));
            }
            texture = source.texture;
            return;
        }
        if (source.kind != Value::Kind::Numeric) {
            throw std::runtime_error(NodeError(
                node, "has invalid " + std::string(role)));
        }
        target = source.number[0];
    }

    static void ApplyScalarWeight(
        const Value& source, float& target, std::string& texture,
        const MaterialXProgramNode& node, std::string_view role)
    {
        if (target <= 0.0F && texture.empty()) {
            AssignScalar(source, target, texture, node, role);
            return;
        }
        if (source.kind == Value::Kind::Numeric) {
            if (!texture.empty() && !NumericIs(source, 1.0F)) {
                throw std::runtime_error(NodeError(
                    node, "combines a texture-driven mix with dynamic " +
                    std::string(role)));
            }
            if (texture.empty()) target *= source.number[0];
            return;
        }
        throw std::runtime_error(NodeError(
            node, "has multiple texture-driven " + std::string(role) + " terms"));
    }

    static void AssignRoughness(
        const Value& source, float& roughnessU, float& roughnessV,
        std::string& texture, const MaterialXProgramNode& node,
        std::string_view role)
    {
        if (source.kind == Value::Kind::Texture) {
            if (source.textureChannel > 0 || source.textureInverted) {
                throw std::runtime_error(NodeError(
                    node, "uses a packed or inverted " + std::string(role)));
            }
            texture = source.texture;
            return;
        }
        if (source.kind != Value::Kind::Numeric) {
            throw std::runtime_error(NodeError(
                node, "has invalid " + std::string(role)));
        }
        // MaterialX primitive BSDF roughness is the microfacet alpha pair.
        // SceneMaterial retains perceptual roughness for the existing GPU ABI,
        // which squares it again while evaluating GGX.
        roughnessU = std::sqrt(std::clamp(source.number[0], 0.0F, 1.0F));
        roughnessV = std::sqrt(std::clamp(
            source.type == "vector2" ? source.number[1] : source.number[0],
            0.0F, 1.0F));
    }

    static void AssignColor(
        const Value& source, std::array<float, 3>& target, std::string& texture,
        const MaterialXProgramNode& node, std::string_view role)
    {
        if (source.kind == Value::Kind::Texture) {
            if (source.textureChannel >= 0 || source.textureInverted) {
                throw std::runtime_error(NodeError(
                    node, "uses a swizzled or inverted color texture"));
            }
            texture = source.texture;
            return;
        }
        if (source.kind != Value::Kind::Numeric) {
            throw std::runtime_error(NodeError(
                node, "has invalid " + std::string(role)));
        }
        std::copy_n(source.number.begin(), 3U, target.begin());
    }

    const MaterialXGeneratedProgram& program;
    ProgramEvaluator evaluator;
    SceneMaterial material;
    std::vector<Dielectric> dielectrics;
    bool containsDiffuseClosure{false};
};

} // namespace

SceneMaterial CompileMaterialXClosure(
    const MaterialXGeneratedProgram& program,
    std::string_view terminalNodeDef)
{
    return ClosureCompiler(program).Compile(terminalNodeDef);
}

MaterialXDisplacement EvaluateMaterialXDisplacement(
    const MaterialXGeneratedProgram& program,
    const MaterialXEvaluationContext& context)
{
    if (program.outputNode.empty()) {
        throw std::runtime_error(
            "MaterialX displacement program has no output node");
    }
    ProgramEvaluator evaluator(program, &context);
    const MaterialXProgramNode& terminal = evaluator.Node(program.outputNode);
    if (terminal.category != "displacement" &&
        terminal.nodeDef != "ND_displacement_float" &&
        terminal.nodeDef != "ND_displacement_vector3") {
        throw std::runtime_error(NodeError(
            terminal, "is not a MaterialX displacement constructor"));
    }
    const Value displacement = evaluator.Input(terminal, "displacement");
    const Value scale = evaluator.InputOr(terminal, "scale", "float", "1");
    if (displacement.kind != Value::Kind::Numeric ||
        scale.kind != Value::Kind::Numeric) {
        throw std::runtime_error(NodeError(
            terminal, "did not evaluate to numeric displacement"));
    }

    MaterialXDisplacement result;
    result.tangentSpace = ComponentCount(displacement.type) >= 3U;
    if (result.tangentSpace) {
        for (std::size_t component = 0; component < 3U; ++component) {
            result.vector[component] =
                displacement.number[component] * scale.number[0];
        }
    } else {
        result.vector[2] = displacement.number[0] * scale.number[0];
    }
    return result;
}

} // namespace hdcodex
