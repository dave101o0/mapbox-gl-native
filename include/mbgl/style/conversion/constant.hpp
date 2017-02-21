#pragma once

#include <mbgl/style/conversion.hpp>
#include <mbgl/util/optional.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/enum.hpp>

#include <array>
#include <string>
#include <vector>

namespace mbgl {
namespace style {
namespace conversion {

template <>
struct Converter<bool> {
    template <class V>
    optional<bool> operator()(const V& value, Error& error) const {
        optional<bool> converted = toBool(value);
        if (!converted) {
            error = { "value must be a boolean" };
            return {};
        }
        return *converted;
    }
};

template <>
struct Converter<float> {
    template <class V>
    optional<float> operator()(const V& value, Error& error) const {
        optional<float> converted = toNumber(value);
        if (!converted) {
            error = { "value must be a number" };
            return {};
        }
        return *converted;
    }
};

template <>
struct Converter<std::string> {
    template <class V>
    optional<std::string> operator()(const V& value, Error& error) const {
        optional<std::string> converted = toString(value);
        if (!converted) {
            error = { "value must be a string" };
            return {};
        }
        return *converted;
    }
};

template <class T>
struct Converter<T, typename std::enable_if_t<std::is_enum<T>::value>> {
    template <class V>
    optional<T> operator()(const V& value, Error& error) const {
        optional<std::string> string = toString(value);
        if (!string) {
            error = { "value must be a string" };
            return {};
        }

        const auto result = Enum<T>::toEnum(*string);
        if (!result) {
            error = { "value must be a valid enumeration value" };
            return {};
        }

        return *result;
    }
};

template <>
struct Converter<Color> {
    template <class V>
    optional<Color> operator()(const V& value, Error& error) const {
        optional<std::string> string = toString(value);
        if (!string) {
            error = { "value must be a string" };
            return {};
        }

        optional<Color> color = Color::parse(*string);
        if (!color) {
            error = { "value must be a valid color" };
            return {};
        }

        return *color;
    }
};

template <>
struct Converter<std::array<float, 2>> {
    template <class V>
    optional<std::array<float, 2>> operator()(const V& value, Error& error) const {
        if (!isArray(value) || arrayLength(value) != 2) {
            error = { "value must be an array of two numbers" };
            return {};
        }

        optional<float> first = toNumber(arrayMember(value, 0));
        optional<float> second = toNumber(arrayMember(value, 1));
        if (!first || !second) {
            error = { "value must be an array of two numbers" };
            return {};
        }

        return std::array<float, 2> {{ *first, *second }};
    }
};

template <>
struct Converter<std::array<float, 3>> {
    template <class V>
    Result<std::array<float, 3>> operator()(const V& value) const {
        if (!isArray(value) || arrayLength(value) != 3) {
            return Error { "value must be an array of three numbers" };
        }

        optional<float> first = toNumber(arrayMember(value, 0));
        optional<float> second = toNumber(arrayMember(value, 1));
        optional<float> third = toNumber(arrayMember(value, 2));
        if (!first || !second || !third) {
            return Error { "value must be an array of three numbers" };
        }

        return std::array<float, 3> {{ *first, *second, *third }};
    }
};

template <>
struct Converter<std::array<float, 4>> {
    template <class V>
    optional<std::array<float, 4>> operator()(const V& value, Error& error) const {
        if (!isArray(value) || arrayLength(value) != 4) {
            error = { "value must be an array of four numbers" };
            return {};
        }

        optional<float> first = toNumber(arrayMember(value, 0));
        optional<float> second = toNumber(arrayMember(value, 1));
        optional<float> third = toNumber(arrayMember(value, 2));
        optional<float> fourth = toNumber(arrayMember(value, 3));
        if (!first || !second) {
            error = { "value must be an array of four numbers" };
            return {};
        }

        return std::array<float, 4> {{ *first, *second, *third, *fourth }};
    }
};

template <>
struct Converter<std::vector<float>> {
    template <class V>
    optional<std::vector<float>> operator()(const V& value, Error& error) const {
        if (!isArray(value)) {
            error = { "value must be an array" };
            return {};
        }

        std::vector<float> result;
        result.reserve(arrayLength(value));

        for (std::size_t i = 0; i < arrayLength(value); ++i) {
            optional<float> number = toNumber(arrayMember(value, i));
            if (!number) {
                error = { "value must be an array of numbers" };
                return {};
            }
            result.push_back(*number);
        }

        return result;
    }
};

template <>
struct Converter<std::vector<std::string>> {
    template <class V>
    optional<std::vector<std::string>> operator()(const V& value, Error& error) const {
        if (!isArray(value)) {
            error = { "value must be an array" };
            return {};
        }

        std::vector<std::string> result;
        result.reserve(arrayLength(value));

        for (std::size_t i = 0; i < arrayLength(value); ++i) {
            optional<std::string> string = toString(arrayMember(value, i));
            if (!string) {
                error = { "value must be an array of strings" };
                return {};
            }
            result.push_back(*string);
        }

        return result;
    }
};

} // namespace conversion
} // namespace style
} // namespace mbgl
