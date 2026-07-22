#include "GltfLoader.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using Object = std::unordered_map<std::string, struct JsonValue>;

struct JsonValue final
{
    enum class Type
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    Object object;
};

class JsonParser final
{
public:
    explicit JsonParser(std::string_view source) noexcept
        : source_(source)
    {
    }

    [[nodiscard]] JsonValue Parse()
    {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();

        if (position_ != source_.size())
        {
            Fail("Unexpected characters after the root value");
        }

        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const
    {
        throw std::runtime_error(
            "glTF JSON error at byte " +
            std::to_string(position_) +
            ": " + message
        );
    }

    void SkipWhitespace() noexcept
    {
        while (position_ < source_.size())
        {
            const char character = source_[position_];
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n')
            {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] JsonValue ParseValue()
    {
        if (position_ >= source_.size())
        {
            Fail("Expected a JSON value");
        }

        switch (source_[position_])
        {
        case '{':
            return ParseObject();
        case '[':
            return ParseArray();
        case '"':
        {
            JsonValue value;
            value.type = JsonValue::Type::String;
            value.string = ParseString();
            return value;
        }
        case 't':
            return ParseLiteral("true", JsonValue::Type::Boolean, true);
        case 'f':
            return ParseLiteral("false", JsonValue::Type::Boolean, false);
        case 'n':
            return ParseLiteral("null", JsonValue::Type::Null, false);
        default:
            if (source_[position_] == '-' ||
                (source_[position_] >= '0' && source_[position_] <= '9'))
            {
                return ParseNumber();
            }
            Fail("Invalid JSON value");
        }
    }

    [[nodiscard]] JsonValue ParseObject()
    {
        JsonValue value;
        value.type = JsonValue::Type::Object;
        ++position_;
        SkipWhitespace();

        if (Consume('}'))
        {
            return value;
        }

        while (true)
        {
            if (position_ >= source_.size() || source_[position_] != '"')
            {
                Fail("Expected an object member name");
            }

            std::string name = ParseString();
            SkipWhitespace();
            if (!Consume(':'))
            {
                Fail("Expected ':' after an object member name");
            }
            SkipWhitespace();

            auto [iterator, inserted] = value.object.emplace(
                std::move(name),
                ParseValue()
            );
            if (!inserted)
            {
                Fail("Duplicate object member: " + iterator->first);
            }

            SkipWhitespace();
            if (Consume('}'))
            {
                break;
            }
            if (!Consume(','))
            {
                Fail("Expected ',' or '}' in an object");
            }
            SkipWhitespace();
        }

        return value;
    }

    [[nodiscard]] JsonValue ParseArray()
    {
        JsonValue value;
        value.type = JsonValue::Type::Array;
        ++position_;
        SkipWhitespace();

        if (Consume(']'))
        {
            return value;
        }

        while (true)
        {
            value.array.push_back(ParseValue());
            SkipWhitespace();
            if (Consume(']'))
            {
                break;
            }
            if (!Consume(','))
            {
                Fail("Expected ',' or ']' in an array");
            }
            SkipWhitespace();
        }

        return value;
    }

    [[nodiscard]] std::string ParseString()
    {
        if (!Consume('"'))
        {
            Fail("Expected a string");
        }

        std::string result;
        while (position_ < source_.size())
        {
            const unsigned char character =
                static_cast<unsigned char>(source_[position_++]);

            if (character == '"')
            {
                return result;
            }
            if (character < 0x20)
            {
                Fail("Unescaped control character in a string");
            }
            if (character != '\\')
            {
                result.push_back(static_cast<char>(character));
                continue;
            }

            if (position_ >= source_.size())
            {
                Fail("Incomplete string escape");
            }

            const char escape = source_[position_++];
            switch (escape)
            {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
                AppendUnicodeEscape(result);
                break;
            default:
                Fail("Invalid string escape");
            }
        }

        Fail("Unterminated string");
    }

    void AppendUnicodeEscape(std::string& destination)
    {
        if (position_ + 4 > source_.size())
        {
            Fail("Incomplete Unicode escape");
        }

        std::uint32_t codePoint = 0;
        for (int digitIndex = 0; digitIndex < 4; ++digitIndex)
        {
            const char digit = source_[position_++];
            codePoint <<= 4;
            if (digit >= '0' && digit <= '9')
            {
                codePoint += static_cast<std::uint32_t>(digit - '0');
            }
            else if (digit >= 'a' && digit <= 'f')
            {
                codePoint += static_cast<std::uint32_t>(digit - 'a' + 10);
            }
            else if (digit >= 'A' && digit <= 'F')
            {
                codePoint += static_cast<std::uint32_t>(digit - 'A' + 10);
            }
            else
            {
                Fail("Invalid hexadecimal digit in Unicode escape");
            }
        }

        if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
        {
            Fail("UTF-16 surrogate escapes are outside this milestone subset");
        }

        if (codePoint <= 0x7F)
        {
            destination.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FF)
        {
            destination.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            destination.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            destination.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            destination.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F))
            );
            destination.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    [[nodiscard]] JsonValue ParseNumber()
    {
        const std::size_t start = position_;

        Consume('-');
        if (Consume('0'))
        {
            if (position_ < source_.size() &&
                source_[position_] >= '0' && source_[position_] <= '9')
            {
                Fail("Leading zero in a number");
            }
        }
        else
        {
            ParseDigits();
        }

        if (Consume('.'))
        {
            ParseDigits();
        }
        if (position_ < source_.size() &&
            (source_[position_] == 'e' || source_[position_] == 'E'))
        {
            ++position_;
            if (position_ < source_.size() &&
                (source_[position_] == '+' || source_[position_] == '-'))
            {
                ++position_;
            }
            ParseDigits();
        }

        const std::string numberText(source_.substr(start, position_ - start));
        char* parseEnd = nullptr;
        const double number = std::strtod(numberText.c_str(), &parseEnd);
        if (parseEnd != numberText.c_str() + numberText.size() ||
            !std::isfinite(number))
        {
            Fail("Invalid or non-finite number");
        }

        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.number = number;
        return value;
    }

    void ParseDigits()
    {
        const std::size_t start = position_;
        while (position_ < source_.size() &&
               source_[position_] >= '0' && source_[position_] <= '9')
        {
            ++position_;
        }
        if (start == position_)
        {
            Fail("Expected a decimal digit");
        }
    }

    [[nodiscard]] JsonValue ParseLiteral(
        std::string_view literal,
        JsonValue::Type type,
        bool boolean)
    {
        if (source_.substr(position_, literal.size()) != literal)
        {
            Fail("Invalid JSON literal");
        }
        position_ += literal.size();

        JsonValue value;
        value.type = type;
        value.boolean = boolean;
        return value;
    }

    bool Consume(char expected) noexcept
    {
        if (position_ < source_.size() && source_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

[[nodiscard]] const JsonValue& RequireMember(
    const JsonValue& object,
    std::string_view name)
{
    if (object.type != JsonValue::Type::Object)
    {
        throw std::runtime_error("Expected a glTF JSON object.");
    }

    const auto iterator = object.object.find(std::string(name));
    if (iterator == object.object.end())
    {
        throw std::runtime_error(
            "Required glTF member is missing: " + std::string(name)
        );
    }
    return iterator->second;
}

[[nodiscard]] const JsonValue* FindMember(
    const JsonValue& object,
    std::string_view name)
{
    if (object.type != JsonValue::Type::Object)
    {
        throw std::runtime_error("Expected a glTF JSON object.");
    }

    const auto iterator = object.object.find(std::string(name));
    return iterator == object.object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] const std::vector<JsonValue>& RequireArray(
    const JsonValue& value,
    std::string_view label)
{
    if (value.type != JsonValue::Type::Array)
    {
        throw std::runtime_error(
            "glTF member must be an array: " + std::string(label)
        );
    }
    return value.array;
}

[[nodiscard]] std::string RequireString(
    const JsonValue& value,
    std::string_view label)
{
    if (value.type != JsonValue::Type::String)
    {
        throw std::runtime_error(
            "glTF member must be a string: " + std::string(label)
        );
    }
    return value.string;
}

[[nodiscard]] std::size_t RequireIndex(
    const JsonValue& value,
    std::string_view label)
{
    if (value.type != JsonValue::Type::Number || value.number < 0.0 ||
        std::floor(value.number) != value.number ||
        value.number > static_cast<double>(
            std::numeric_limits<std::size_t>::max()
        ))
    {
        throw std::runtime_error(
            "glTF member must be a non-negative integer: " +
            std::string(label)
        );
    }
    return static_cast<std::size_t>(value.number);
}

[[nodiscard]] std::size_t OptionalIndex(
    const JsonValue& object,
    std::string_view name,
    std::size_t fallback)
{
    const JsonValue* value = FindMember(object, name);
    return value == nullptr ? fallback : RequireIndex(*value, name);
}

[[nodiscard]] double RequireNumber(
    const JsonValue& value,
    std::string_view label)
{
    if (value.type != JsonValue::Type::Number)
    {
        throw std::runtime_error(
            "glTF member must be a number: " + std::string(label)
        );
    }
    return value.number;
}

[[nodiscard]] bool RequireBoolean(
    const JsonValue& value,
    std::string_view label)
{
    if (value.type != JsonValue::Type::Boolean)
    {
        throw std::runtime_error(
            "glTF member must be a boolean: " + std::string(label)
        );
    }
    return value.boolean;
}

[[nodiscard]] std::string ReadTextFile(
    const std::filesystem::path& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error(
            "Failed to open glTF file: " + filePath.string()
        );
    }

    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

[[nodiscard]] std::vector<std::uint8_t> ReadBinaryFile(
    const std::filesystem::path& filePath)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error(
            "Failed to open glTF buffer: " + filePath.string()
        );
    }

    const std::streampos end = file.tellg();
    if (end < 0)
    {
        throw std::runtime_error(
            "Failed to determine glTF buffer size: " + filePath.string()
        );
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !file.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        ))
    {
        throw std::runtime_error(
            "Failed to read complete glTF buffer: " + filePath.string()
        );
    }
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> DecodeBase64(
    std::string_view encoded)
{
    static constexpr std::array<std::int8_t, 256> decodeTable = []
    {
        std::array<std::int8_t, 256> table{};
        table.fill(-1);
        for (int value = 0; value < 26; ++value)
        {
            table[static_cast<std::size_t>('A' + value)] =
                static_cast<std::int8_t>(value);
            table[static_cast<std::size_t>('a' + value)] =
                static_cast<std::int8_t>(value + 26);
        }
        for (int value = 0; value < 10; ++value)
        {
            table[static_cast<std::size_t>('0' + value)] =
                static_cast<std::int8_t>(value + 52);
        }
        table[static_cast<std::size_t>('+')] = 62;
        table[static_cast<std::size_t>('/')] = 63;
        return table;
    }();

    if (encoded.size() % 4 != 0)
    {
        throw std::runtime_error("Base64 glTF buffer length is invalid.");
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded.size() / 4 * 3);

    for (std::size_t offset = 0; offset < encoded.size(); offset += 4)
    {
        std::uint32_t accumulator = 0;
        int padding = 0;
        for (int characterIndex = 0; characterIndex < 4; ++characterIndex)
        {
            const unsigned char character = static_cast<unsigned char>(
                encoded[offset + static_cast<std::size_t>(characterIndex)]
            );
            if (character == '=')
            {
                ++padding;
                accumulator <<= 6;
                continue;
            }
            if (padding != 0 || decodeTable[character] < 0)
            {
                throw std::runtime_error(
                    "Base64 glTF buffer contains invalid padding or characters."
                );
            }
            accumulator =
                (accumulator << 6) |
                static_cast<std::uint32_t>(decodeTable[character]);
        }

        if (padding > 2 || (padding != 0 && offset + 4 != encoded.size()))
        {
            throw std::runtime_error("Base64 glTF buffer padding is invalid.");
        }

        decoded.push_back(static_cast<std::uint8_t>(accumulator >> 16));
        if (padding < 2)
        {
            decoded.push_back(static_cast<std::uint8_t>(accumulator >> 8));
        }
        if (padding < 1)
        {
            decoded.push_back(static_cast<std::uint8_t>(accumulator));
        }
    }

    return decoded;
}

[[nodiscard]] std::vector<std::uint8_t> LoadBufferUri(
    const std::filesystem::path& gltfDirectory,
    const std::string& uri)
{
    constexpr std::string_view dataPrefix =
        "data:application/octet-stream;base64,";
    constexpr std::string_view genericDataPrefix =
        "data:application/gltf-buffer;base64,";

    if (uri.starts_with(dataPrefix))
    {
        return DecodeBase64(std::string_view(uri).substr(dataPrefix.size()));
    }
    if (uri.starts_with(genericDataPrefix))
    {
        return DecodeBase64(
            std::string_view(uri).substr(genericDataPrefix.size())
        );
    }
    if (uri.starts_with("data:"))
    {
        throw std::runtime_error(
            "Only base64 glTF binary data URIs are supported."
        );
    }
    if (uri.find('%') != std::string::npos)
    {
        throw std::runtime_error(
            "Percent-encoded external glTF buffer URIs are not supported yet."
        );
    }

    return ReadBinaryFile(gltfDirectory / std::filesystem::path(uri));
}

struct BufferViewInfo final
{
    std::size_t buffer = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t stride = 0;
};

struct AccessorInfo final
{
    std::size_t bufferView = 0;
    std::size_t offset = 0;
    std::size_t count = 0;
    std::size_t componentType = 0;
    std::string type;
};

[[nodiscard]] std::size_t ComponentByteSize(std::size_t componentType)
{
    switch (componentType)
    {
    case 5121: return sizeof(std::uint8_t);
    case 5123: return sizeof(std::uint16_t);
    case 5125: return sizeof(std::uint32_t);
    case 5126: return sizeof(float);
    default:
        throw std::runtime_error(
            "Unsupported glTF accessor componentType: " +
            std::to_string(componentType)
        );
    }
}

[[nodiscard]] std::size_t ComponentCount(std::string_view type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    throw std::runtime_error(
        "Unsupported glTF accessor type: " + std::string(type)
    );
}

[[nodiscard]] const std::uint8_t* AccessorElement(
    const AccessorInfo& accessor,
    const std::vector<BufferViewInfo>& bufferViews,
    const std::vector<std::vector<std::uint8_t>>& buffers,
    std::size_t elementIndex)
{
    if (accessor.bufferView >= bufferViews.size())
    {
        throw std::runtime_error("glTF accessor bufferView index is invalid.");
    }

    const BufferViewInfo& view = bufferViews[accessor.bufferView];
    if (view.buffer >= buffers.size())
    {
        throw std::runtime_error("glTF bufferView buffer index is invalid.");
    }
    if (elementIndex >= accessor.count)
    {
        throw std::runtime_error("glTF accessor element index is invalid.");
    }

    const std::size_t elementSize =
        ComponentByteSize(accessor.componentType) *
        ComponentCount(accessor.type);
    const std::size_t stride = view.stride == 0 ? elementSize : view.stride;

    if (stride < elementSize)
    {
        throw std::runtime_error(
            "glTF bufferView byteStride is smaller than its accessor element."
        );
    }
    if (elementIndex >
        (std::numeric_limits<std::size_t>::max() - accessor.offset) / stride)
    {
        throw std::runtime_error("glTF accessor byte offset overflow.");
    }

    const std::size_t relativeOffset = accessor.offset + elementIndex * stride;
    if (relativeOffset > view.length ||
        elementSize > view.length - relativeOffset)
    {
        throw std::runtime_error(
            "glTF accessor reads beyond its bufferView."
        );
    }
    if (view.offset > buffers[view.buffer].size() ||
        relativeOffset > buffers[view.buffer].size() - view.offset ||
        elementSize > buffers[view.buffer].size() - view.offset - relativeOffset)
    {
        throw std::runtime_error("glTF accessor reads beyond its buffer.");
    }

    return buffers[view.buffer].data() + view.offset + relativeOffset;
}

template <std::size_t ComponentCountValue>
[[nodiscard]] std::array<float, ComponentCountValue> ReadFloatAttribute(
    const AccessorInfo& accessor,
    const std::vector<BufferViewInfo>& bufferViews,
    const std::vector<std::vector<std::uint8_t>>& buffers,
    std::size_t elementIndex,
    std::string_view semantic)
{
    if (accessor.componentType != 5126 ||
        ComponentCount(accessor.type) != ComponentCountValue)
    {
        throw std::runtime_error(
            "glTF " + std::string(semantic) +
            " must use FLOAT with the expected vector size."
        );
    }

    std::array<float, ComponentCountValue> result{};
    std::memcpy(
        result.data(),
        AccessorElement(accessor, bufferViews, buffers, elementIndex),
        sizeof(result)
    );
    for (float value : result)
    {
        if (!std::isfinite(value))
        {
            throw std::runtime_error(
                "glTF " + std::string(semantic) +
                " contains a non-finite value."
            );
        }
    }
    return result;
}

[[nodiscard]] std::uint32_t ReadIndex(
    const AccessorInfo& accessor,
    const std::vector<BufferViewInfo>& bufferViews,
    const std::vector<std::vector<std::uint8_t>>& buffers,
    std::size_t elementIndex)
{
    if (accessor.type != "SCALAR")
    {
        throw std::runtime_error("glTF indices accessor must be SCALAR.");
    }

    const std::uint8_t* source =
        AccessorElement(accessor, bufferViews, buffers, elementIndex);
    switch (accessor.componentType)
    {
    case 5121:
        return *source;
    case 5123:
    {
        std::uint16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    case 5125:
    {
        std::uint32_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    default:
        throw std::runtime_error(
            "glTF indices must use UNSIGNED_BYTE, UNSIGNED_SHORT, or "
            "UNSIGNED_INT."
        );
    }
}

[[nodiscard]] DirectX::XMMATRIX ReadNodeLocalTransform(
    const JsonValue& node)
{
    if (const JsonValue* matrix = FindMember(node, "matrix"))
    {
        const auto& values = RequireArray(*matrix, "node.matrix");
        if (values.size() != 16)
        {
            throw std::runtime_error("glTF node matrix must contain 16 values.");
        }

        DirectX::XMFLOAT4X4 rowMatrix{};
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                // A glTF matrix is serialized column-major. Reading that
                // same linear sequence as a DirectX row-major matrix gives
                // the transpose required for row-vector multiplication.
                rowMatrix.m[row][column] = static_cast<float>(RequireNumber(
                    values[row * 4 + column],
                    "node.matrix element"
                ));
            }
        }
        return DirectX::XMLoadFloat4x4(&rowMatrix);
    }

    DirectX::XMFLOAT3 translation{0.0F, 0.0F, 0.0F};
    DirectX::XMFLOAT4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    DirectX::XMFLOAT3 scale{1.0F, 1.0F, 1.0F};

    const auto readVector = [](const JsonValue& value, float* destination,
                               std::size_t count, std::string_view label)
    {
        const auto& values = RequireArray(value, label);
        if (values.size() != count)
        {
            throw std::runtime_error(
                "glTF " + std::string(label) + " has an invalid size."
            );
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            destination[index] = static_cast<float>(
                RequireNumber(values[index], label)
            );
        }
    };

    if (const JsonValue* value = FindMember(node, "translation"))
    {
        readVector(*value, &translation.x, 3, "node.translation");
    }
    if (const JsonValue* value = FindMember(node, "rotation"))
    {
        readVector(*value, &rotation.x, 4, "node.rotation");
    }
    if (const JsonValue* value = FindMember(node, "scale"))
    {
        readVector(*value, &scale.x, 3, "node.scale");
    }

    const DirectX::XMVECTOR quaternion = DirectX::XMLoadFloat4(&rotation);
    const float quaternionLengthSquared =
        DirectX::XMVectorGetX(DirectX::XMVector4LengthSq(quaternion));
    if (!std::isfinite(quaternionLengthSquared) ||
        quaternionLengthSquared <= 0.0F)
    {
        throw std::runtime_error("glTF node rotation quaternion is invalid.");
    }

    return
        DirectX::XMMatrixScaling(scale.x, scale.y, scale.z) *
        DirectX::XMMatrixRotationQuaternion(
            DirectX::XMQuaternionNormalize(quaternion)
        ) *
        DirectX::XMMatrixTranslation(
            translation.x,
            translation.y,
            translation.z
        );
}

struct DrawableNode final
{
    std::size_t meshIndex = 0;
    DirectX::XMFLOAT4X4 worldTransform{};
};

void VisitNode(
    std::size_t nodeIndex,
    DirectX::FXMMATRIX parentTransform,
    const std::vector<JsonValue>& nodes,
    std::vector<bool>& activeNodes,
    std::vector<DrawableNode>& drawableNodes)
{
    if (nodeIndex >= nodes.size())
    {
        throw std::runtime_error("glTF scene node index is invalid.");
    }
    if (activeNodes[nodeIndex])
    {
        throw std::runtime_error("glTF node hierarchy contains a cycle.");
    }

    activeNodes[nodeIndex] = true;
    const JsonValue& node = nodes[nodeIndex];
    const DirectX::XMMATRIX worldTransform =
        ReadNodeLocalTransform(node) * parentTransform;

    if (const JsonValue* mesh = FindMember(node, "mesh"))
    {
        DrawableNode drawable;
        drawable.meshIndex = RequireIndex(*mesh, "node.mesh");
        DirectX::XMStoreFloat4x4(&drawable.worldTransform, worldTransform);
        drawableNodes.push_back(drawable);
    }

    if (const JsonValue* children = FindMember(node, "children"))
    {
        for (const JsonValue& child : RequireArray(*children, "node.children"))
        {
            VisitNode(
                RequireIndex(child, "node child"),
                worldTransform,
                nodes,
                activeNodes,
                drawableNodes
            );
        }
    }
    activeNodes[nodeIndex] = false;
}

} // namespace

namespace dx12
{

StaticModelData LoadStaticGltfModel(const std::filesystem::path& filePath)
{
    if (filePath.extension() != ".gltf")
    {
        throw std::runtime_error(
            "The current static-mesh milestone supports .gltf files only: " +
            filePath.string()
        );
    }

    const JsonValue root = JsonParser(ReadTextFile(filePath)).Parse();
    const JsonValue& asset = RequireMember(root, "asset");
    const std::string version = RequireString(
        RequireMember(asset, "version"),
        "asset.version"
    );
    if (version != "2.0")
    {
        throw std::runtime_error(
            "Only glTF 2.0 is supported; asset.version is " + version
        );
    }

    const auto& bufferJson = RequireArray(
        RequireMember(root, "buffers"),
        "buffers"
    );
    std::vector<std::vector<std::uint8_t>> buffers;
    buffers.reserve(bufferJson.size());
    for (const JsonValue& buffer : bufferJson)
    {
        const std::size_t declaredLength = RequireIndex(
            RequireMember(buffer, "byteLength"),
            "buffer.byteLength"
        );
        const std::string uri = RequireString(
            RequireMember(buffer, "uri"),
            "buffer.uri"
        );
        std::vector<std::uint8_t> bytes =
            LoadBufferUri(filePath.parent_path(), uri);
        if (bytes.size() < declaredLength)
        {
            throw std::runtime_error(
                "glTF buffer contains fewer bytes than buffer.byteLength."
            );
        }
        buffers.push_back(std::move(bytes));
    }

    const auto& bufferViewJson = RequireArray(
        RequireMember(root, "bufferViews"),
        "bufferViews"
    );
    std::vector<BufferViewInfo> bufferViews;
    bufferViews.reserve(bufferViewJson.size());
    for (const JsonValue& view : bufferViewJson)
    {
        BufferViewInfo info;
        info.buffer = RequireIndex(
            RequireMember(view, "buffer"),
            "bufferView.buffer"
        );
        info.offset = OptionalIndex(view, "byteOffset", 0);
        info.length = RequireIndex(
            RequireMember(view, "byteLength"),
            "bufferView.byteLength"
        );
        info.stride = OptionalIndex(view, "byteStride", 0);
        if (info.buffer >= buffers.size() ||
            info.offset > buffers[info.buffer].size() ||
            info.length > buffers[info.buffer].size() - info.offset)
        {
            throw std::runtime_error("glTF bufferView exceeds its buffer.");
        }
        bufferViews.push_back(info);
    }

    const auto& accessorJson = RequireArray(
        RequireMember(root, "accessors"),
        "accessors"
    );
    std::vector<AccessorInfo> accessors;
    accessors.reserve(accessorJson.size());
    for (const JsonValue& accessor : accessorJson)
    {
        if (FindMember(accessor, "sparse") != nullptr)
        {
            throw std::runtime_error(
                "Sparse glTF accessors are outside this milestone subset."
            );
        }

        AccessorInfo info;
        info.bufferView = RequireIndex(
            RequireMember(accessor, "bufferView"),
            "accessor.bufferView"
        );
        info.offset = OptionalIndex(accessor, "byteOffset", 0);
        info.componentType = RequireIndex(
            RequireMember(accessor, "componentType"),
            "accessor.componentType"
        );
        info.count = RequireIndex(
            RequireMember(accessor, "count"),
            "accessor.count"
        );
        info.type = RequireString(
            RequireMember(accessor, "type"),
            "accessor.type"
        );
        if (info.count == 0)
        {
            throw std::runtime_error("glTF accessor count cannot be zero.");
        }
        accessors.push_back(std::move(info));
    }

    const auto& nodes = RequireArray(RequireMember(root, "nodes"), "nodes");
    const auto& scenes = RequireArray(RequireMember(root, "scenes"), "scenes");
    const std::size_t sceneIndex = OptionalIndex(root, "scene", 0);
    if (sceneIndex >= scenes.size())
    {
        throw std::runtime_error("glTF default scene index is invalid.");
    }

    std::vector<bool> activeNodes(nodes.size(), false);
    std::vector<DrawableNode> drawableNodes;
    const auto& sceneRoots = RequireArray(
        RequireMember(scenes[sceneIndex], "nodes"),
        "scene.nodes"
    );
    for (const JsonValue& sceneRoot : sceneRoots)
    {
        VisitNode(
            RequireIndex(sceneRoot, "scene root node"),
            DirectX::XMMatrixIdentity(),
            nodes,
            activeNodes,
            drawableNodes
        );
    }
    if (drawableNodes.size() != 1)
    {
        throw std::runtime_error(
            "This milestone requires exactly one drawable glTF mesh node."
        );
    }

    const auto& meshes = RequireArray(RequireMember(root, "meshes"), "meshes");
    const DrawableNode& drawable = drawableNodes.front();
    if (drawable.meshIndex >= meshes.size())
    {
        throw std::runtime_error("glTF node mesh index is invalid.");
    }

    const auto& primitives = RequireArray(
        RequireMember(meshes[drawable.meshIndex], "primitives"),
        "mesh.primitives"
    );
    if (primitives.size() != 1)
    {
        throw std::runtime_error(
            "This milestone requires exactly one glTF mesh primitive."
        );
    }
    const JsonValue& primitive = primitives.front();
    if (OptionalIndex(primitive, "mode", 4) != 4)
    {
        throw std::runtime_error(
            "The glTF primitive must use TRIANGLES mode."
        );
    }

    const std::size_t materialIndex = RequireIndex(
        RequireMember(primitive, "material"),
        "primitive.material"
    );
    const auto& materials = RequireArray(
        RequireMember(root, "materials"),
        "materials"
    );
    if (materialIndex >= materials.size())
    {
        throw std::runtime_error("glTF primitive material index is invalid.");
    }

    const std::vector<JsonValue>* textures = nullptr;
    if (const JsonValue* textureArray = FindMember(root, "textures"))
    {
        textures = &RequireArray(*textureArray, "textures");
    }

    const std::vector<JsonValue>* images = nullptr;
    if (const JsonValue* imageArray = FindMember(root, "images"))
    {
        images = &RequireArray(*imageArray, "images");
    }

    const auto readUnitFactor = [](
        const JsonValue& value,
        std::string_view label) -> float
    {
        const double factor = RequireNumber(value, label);
        if (factor < 0.0 || factor > 1.0)
        {
            throw std::runtime_error(
                "glTF " + std::string(label) +
                " must be between 0 and 1."
            );
        }
        return static_cast<float>(factor);
    };

    const auto resolveTextureSource = [textures, images, &filePath](
        const JsonValue& textureInfo,
        std::string_view label) -> MaterialTextureSource
    {
        if (textures == nullptr || images == nullptr)
        {
            throw std::runtime_error(
                "A glTF material texture requires root textures and images "
                "arrays."
            );
        }

        const std::size_t textureCoordinateSet =
            OptionalIndex(textureInfo, "texCoord", 0);
        if (textureCoordinateSet != 0)
        {
            throw std::runtime_error(
                "glTF " + std::string(label) +
                " must use TEXCOORD_0 in this milestone."
            );
        }

        const std::size_t textureIndex = RequireIndex(
            RequireMember(textureInfo, "index"),
            std::string(label) + ".index"
        );
        if (textureIndex >= textures->size())
        {
            throw std::runtime_error(
                "glTF " + std::string(label) +
                " texture index is invalid."
            );
        }

        const std::size_t imageIndex = RequireIndex(
            RequireMember((*textures)[textureIndex], "source"),
            "texture.source"
        );
        if (imageIndex >= images->size())
        {
            throw std::runtime_error(
                "glTF " + std::string(label) +
                " image source index is invalid."
            );
        }
        if (FindMember((*images)[imageIndex], "bufferView") != nullptr)
        {
            throw std::runtime_error(
                "BufferView-backed glTF images are not supported yet."
            );
        }

        const std::string imageUri = RequireString(
            RequireMember((*images)[imageIndex], "uri"),
            "image.uri"
        );
        if (textureIndex > std::numeric_limits<std::uint32_t>::max() ||
            imageIndex > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "glTF material texture or image index exceeds uint32_t."
            );
        }

        MaterialTextureSource source;
        source.textureIndex = static_cast<std::uint32_t>(textureIndex);
        source.imageIndex = static_cast<std::uint32_t>(imageIndex);
        source.textureCoordinateSet =
            static_cast<std::uint32_t>(textureCoordinateSet);

        constexpr std::string_view pngDataPrefix =
            "data:image/png;base64,";
        constexpr std::string_view jpegDataPrefix =
            "data:image/jpeg;base64,";
        if (imageUri.starts_with(pngDataPrefix))
        {
            source.mimeType = "image/png";
            source.embeddedImageBytes = DecodeBase64(
                std::string_view(imageUri).substr(pngDataPrefix.size())
            );
        }
        else if (imageUri.starts_with(jpegDataPrefix))
        {
            source.mimeType = "image/jpeg";
            source.embeddedImageBytes = DecodeBase64(
                std::string_view(imageUri).substr(jpegDataPrefix.size())
            );
        }
        else
        {
            if (imageUri.starts_with("data:"))
            {
                throw std::runtime_error(
                    "Only base64 PNG and JPEG glTF image Data URIs are "
                    "supported."
                );
            }
            if (imageUri.find('%') != std::string::npos)
            {
                throw std::runtime_error(
                    "Percent-encoded glTF image URIs are not supported yet."
                );
            }
            source.imagePath =
                (filePath.parent_path() / std::filesystem::path(imageUri))
                    .lexically_normal();
        }

        return source;
    };

    StaticMaterialData material;
    const JsonValue& materialJson = materials[materialIndex];
    if (const JsonValue* doubleSided = FindMember(
            materialJson,
            "doubleSided"))
    {
        if (RequireBoolean(*doubleSided, "material.doubleSided"))
        {
            throw std::runtime_error(
                "Double-sided glTF materials are not supported by the "
                "current single-sided Forward PBR milestone."
            );
        }
    }
    if (const JsonValue* pbr = FindMember(
            materialJson,
            "pbrMetallicRoughness"))
    {
        if (const JsonValue* factor = FindMember(*pbr, "baseColorFactor"))
        {
            const auto& values = RequireArray(*factor, "baseColorFactor");
            if (values.size() != 4)
            {
                throw std::runtime_error(
                    "glTF baseColorFactor must contain four values."
                );
            }
            material.baseColorFactor = DirectX::XMFLOAT4{
                readUnitFactor(values[0], "baseColorFactor[0]"),
                readUnitFactor(values[1], "baseColorFactor[1]"),
                readUnitFactor(values[2], "baseColorFactor[2]"),
                readUnitFactor(values[3], "baseColorFactor[3]")
            };
        }
        if (const JsonValue* factor = FindMember(*pbr, "metallicFactor"))
        {
            material.metallicFactor =
                readUnitFactor(*factor, "metallicFactor");
        }
        if (const JsonValue* factor = FindMember(*pbr, "roughnessFactor"))
        {
            material.roughnessFactor =
                readUnitFactor(*factor, "roughnessFactor");
        }
        if (const JsonValue* texture = FindMember(*pbr, "baseColorTexture"))
        {
            material.baseColorTexture = resolveTextureSource(
                *texture,
                "baseColorTexture"
            );
        }
        if (const JsonValue* texture = FindMember(
                *pbr,
                "metallicRoughnessTexture"))
        {
            material.metallicRoughnessTexture = resolveTextureSource(
                *texture,
                "metallicRoughnessTexture"
            );
        }
    }

    if (const JsonValue* normalTexture = FindMember(
            materialJson,
            "normalTexture"))
    {
        material.normalTexture = resolveTextureSource(
            *normalTexture,
            "normalTexture"
        );
        if (const JsonValue* scale = FindMember(*normalTexture, "scale"))
        {
            const double normalScale = RequireNumber(*scale, "normalTexture.scale");
            if (!std::isfinite(normalScale))
            {
                throw std::runtime_error(
                    "glTF normalTexture.scale must be finite."
                );
            }
            material.normalScale = static_cast<float>(normalScale);
        }
    }

    const JsonValue& attributes = RequireMember(primitive, "attributes");
    const auto requireAccessor = [&](std::string_view semantic)
        -> const AccessorInfo&
    {
        const std::size_t index = RequireIndex(
            RequireMember(attributes, semantic),
            semantic
        );
        if (index >= accessors.size())
        {
            throw std::runtime_error(
                "glTF attribute accessor index is invalid: " +
                std::string(semantic)
            );
        }
        return accessors[index];
    };

    const AccessorInfo& positions = requireAccessor("POSITION");
    const AccessorInfo& normals = requireAccessor("NORMAL");
    const AccessorInfo& textureCoordinates = requireAccessor("TEXCOORD_0");
    const AccessorInfo& tangents = requireAccessor("TANGENT");
    if (normals.count != positions.count ||
        textureCoordinates.count != positions.count ||
        tangents.count != positions.count)
    {
        throw std::runtime_error(
            "glTF vertex attribute accessors must have matching counts."
        );
    }

    StaticModelData model;
    model.material = std::move(material);
    MeshData& mesh = model.mesh;
    mesh.vertices.resize(positions.count);
    for (std::size_t vertexIndex = 0;
         vertexIndex < positions.count;
         ++vertexIndex)
    {
        const auto position = ReadFloatAttribute<3>(
            positions, bufferViews, buffers, vertexIndex, "POSITION"
        );
        const auto normal = ReadFloatAttribute<3>(
            normals, bufferViews, buffers, vertexIndex, "NORMAL"
        );
        const auto uv = ReadFloatAttribute<2>(
            textureCoordinates,
            bufferViews,
            buffers,
            vertexIndex,
            "TEXCOORD_0"
        );
        const auto tangent = ReadFloatAttribute<4>(
            tangents, bufferViews, buffers, vertexIndex, "TANGENT"
        );

        Vertex& destination = mesh.vertices[vertexIndex];
        destination.position[0] = position[0];
        destination.position[1] = position[1];
        destination.position[2] = -position[2];
        destination.normal[0] = normal[0];
        destination.normal[1] = normal[1];
        destination.normal[2] = -normal[2];
        destination.textureCoordinates[0] = uv[0];
        destination.textureCoordinates[1] = uv[1];
        destination.tangent[0] = tangent[0];
        destination.tangent[1] = tangent[1];
        destination.tangent[2] = -tangent[2];
        destination.tangent[3] = -tangent[3];
    }

    const std::size_t indicesAccessorIndex = RequireIndex(
        RequireMember(primitive, "indices"),
        "primitive.indices"
    );
    if (indicesAccessorIndex >= accessors.size())
    {
        throw std::runtime_error("glTF indices accessor index is invalid.");
    }
    const AccessorInfo& indices = accessors[indicesAccessorIndex];
    if (indices.count % 3 != 0)
    {
        throw std::runtime_error(
            "glTF TRIANGLES index count must be divisible by three."
        );
    }

    mesh.indices.resize(indices.count);
    for (std::size_t index = 0; index < indices.count; ++index)
    {
        mesh.indices[index] = ReadIndex(
            indices, bufferViews, buffers, index
        );
        if (mesh.indices[index] >= mesh.vertices.size())
        {
            throw std::runtime_error(
                "glTF primitive contains an out-of-range vertex index."
            );
        }
    }

    // Reflecting Z converts glTF's right-handed coordinates to the renderer's
    // left-handed convention. Reverse every triangle so its front face remains
    // front-facing after that reflection.
    for (std::size_t triangle = 0;
         triangle < mesh.indices.size();
         triangle += 3)
    {
        std::swap(mesh.indices[triangle + 1], mesh.indices[triangle + 2]);
    }

    const DirectX::XMMATRIX handedness = DirectX::XMMatrixScaling(1, 1, -1);
    const DirectX::XMMATRIX gltfWorld =
        DirectX::XMLoadFloat4x4(&drawable.worldTransform);
    DirectX::XMStoreFloat4x4(
        &mesh.nodeTransform,
        handedness * gltfWorld * handedness
    );

    return model;
}

} // namespace dx12
