#pragma once
#include <filesystem>
#include <boost/json/value.hpp>

using namespace std::literals::string_view_literals;
using namespace std::string_literals;

namespace fs = std::filesystem;
namespace json = boost::json;

namespace ha {
    struct MallocDeleter{void operator()(void*p)const{std::free(p);}};
    template<typename T>
    struct MallocPtr : std::unique_ptr<T, MallocDeleter> {
	MallocPtr(std::size_t size)
	    : std::unique_ptr<T, MallocDeleter>(allocate(size),
						MallocDeleter{})
	{}
	static T* allocate(std::size_t size) {
	    auto p = std::malloc(size);
	    if(!p) throw std::bad_alloc();
	    return static_cast<T*>(p);
	}
    };

    inline std::size_t block_size() { return 64 * 1024; }
}
