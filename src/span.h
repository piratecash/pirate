// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SPAN_H
#define BITCOIN_SPAN_H

#include <type_traits>
#include <cstddef>
#include <algorithm>
#include <assert.h>

/** A Span is an object that can refer to a contiguous sequence of objects.
 *
 * It implements a subset of C++20's std::span.
 *
 * Things to be aware of when writing code that deals with Spans:
 *
 * - Similar to references themselves, Spans are subject to reference lifetime
 *   issues. The user is responsible for making sure the objects pointed to by
 *   a Span live as long as the Span is used. For example:
 *
 *       std::vector<int> vec{1,2,3,4};
 *       Span<int> sp(vec);
 *       vec.push_back(5);
 *       printf("%i\n", sp.front()); // UB!
 *
 *   may exhibit undefined behavior, as increasing the size of a vector may
 *   invalidate references.
 *
 * - One particular pitfall is that Spans can be constructed from temporaries,
 *   but this is unsafe when the Span is stored in a variable, outliving the
 *   temporary. For example, this will compile, but exhibits undefined behavior:
 *
 *       Span<const int> sp(std::vector<int>{1, 2, 3});
 *       printf("%i\n", sp.front()); // UB!
 *
 *   The lifetime of the vector ends when the statement it is created in ends.
 *   Thus the Span is left with a dangling reference, and using it is undefined.
 *
 * - Due to Span's automatic creation from range-like objects (arrays, and data
 *   types that expose a data() and size() member function), functions that
 *   accept a Span as input parameter can be called with any compatible
 *   range-like object. For example, this works:
*
 *       void Foo(Span<const int> arg);
 *
 *       Foo(std::vector<int>{1, 2, 3}); // Works
 *
 *   This is very useful in cases where a function truly does not care about the
 *   container, and only about having exactly a range of elements. However it
 *   may also be surprising to see automatic conversions in this case.
 *
 *   When a function accepts a Span with a mutable element type, it will not
 *   accept temporaries; only variables or other references. For example:
 *
 *       void FooMut(Span<int> arg);
 *
 *       FooMut(std::vector<int>{1, 2, 3}); // Does not compile
 *       std::vector<int> baz{1, 2, 3};
 *       FooMut(baz); // Works
 *
 *   This is similar to how functions that take (non-const) lvalue references
 *   as input cannot accept temporaries. This does not work either:
 *
 *       void FooVec(std::vector<int>& arg);
 *       FooVec(std::vector<int>{1, 2, 3}); // Does not compile
 *
 *   The idea is that if a function accepts a mutable reference, a meaningful
 *   result will be present in that variable after the call. Passing a temporary
 *   is useless in that context.
 */
template<typename C>
class Span
{
    C* m_data;
    std::ptrdiff_t m_size;

public:
    constexpr Span() noexcept : m_data(nullptr), m_size(0) {}
    constexpr Span(C* data, std::ptrdiff_t size) noexcept : m_data(data), m_size(size) {}
    constexpr Span(C* data, C* end) noexcept : m_data(data), m_size(end - data) {}

    /** Implicit conversion of spans between compatible types.
     *
     *  Specifically, if a pointer to an array of type O can be implicitly converted to a pointer to an array of type
     *  C, then permit implicit conversion of Span<O> to Span<C>. This matches the behavior of the corresponding
     *  C++20 std::span constructor.
     *
     *  For example this means that a Span<T> can be converted into a Span<const T>.
     */
    template <typename O, typename std::enable_if<std::is_convertible<O (*)[], C (*)[]>::value, int>::type = 0>
    constexpr Span(const Span<O>& other) noexcept : m_data(other.m_data), m_size(other.m_size) {}

    /** Default copy constructor. */
    constexpr Span(const Span&) noexcept = default;

    /** Default assignment operator. */
    Span& operator=(const Span& other) noexcept = default;

    constexpr C* data() const noexcept { return m_data; }
    constexpr C* begin() const noexcept { return m_data; }
    constexpr C* end() const noexcept { return m_data + m_size; }
    constexpr C& front() const noexcept { return m_data[0]; }
    constexpr C& back() const noexcept { return m_data[m_size - 1]; }
    constexpr std::ptrdiff_t size() const noexcept { return m_size; }
    constexpr C& operator[](std::ptrdiff_t pos) const noexcept { return m_data[pos]; }

    constexpr Span<C> subspan(std::ptrdiff_t offset) const noexcept { return Span<C>(m_data + offset, m_size - offset); }
    constexpr Span<C> subspan(std::ptrdiff_t offset, std::ptrdiff_t count) const noexcept { return Span<C>(m_data + offset, count); }
    constexpr Span<C> first(std::ptrdiff_t count) const noexcept { return Span<C>(m_data, count); }
    constexpr Span<C> last(std::ptrdiff_t count) const noexcept { return Span<C>(m_data + m_size - count, count); }

    friend constexpr bool operator==(const Span& a, const Span& b) noexcept { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }
    friend constexpr bool operator!=(const Span& a, const Span& b) noexcept { return !(a == b); }
    friend constexpr bool operator<(const Span& a, const Span& b) noexcept { return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end()); }
    friend constexpr bool operator<=(const Span& a, const Span& b) noexcept { return !(b < a); }
    friend constexpr bool operator>(const Span& a, const Span& b) noexcept { return (b < a); }
    friend constexpr bool operator>=(const Span& a, const Span& b) noexcept { return !(a < b); }

    template <typename O> friend class Span;
};

/** Create a span to a container exposing data() and size().
 *
 * This correctly deals with constness: the returned Span's element type will be
 * whatever data() returns a pointer to. If either the passed container is const,
 * or its element type is const, the resulting span will have a const element type.
 *
 * std::span will have a constructor that implements this functionality directly.
 */
template<typename A, int N>
constexpr Span<A> MakeSpan(A (&a)[N]) { return Span<A>(a, N); }

template<typename V>
constexpr Span<typename std::remove_pointer<decltype(std::declval<V>().data())>::type> MakeSpan(V& v) { return Span<typename std::remove_pointer<decltype(std::declval<V>().data())>::type>(v.data(), v.size()); }

/** Pop the last element off a span, and return a reference to that element. */
template <typename T>
T& SpanPopBack(Span<T>& span)
{
    size_t size = span.size();
    assert(size > 0);
    T& back = span[size - 1];
    span = Span<T>(span.data(), size - 1);
    return back;
}

#endif
