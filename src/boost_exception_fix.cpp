// Boost exception handling fix for Windows MSVC
// This file provides the missing boost::throw_exception functions

#include <exception>
#include <boost/config.hpp>

#ifdef BOOST_NO_EXCEPTIONS
#include <cstdlib>
#include <iostream>
#endif

namespace boost {

// Define source_location if not available
#ifndef BOOST_CURRENT_LOCATION
struct source_location {
    constexpr source_location() noexcept = default;
    constexpr source_location(const char* file, int line, const char* function) noexcept
        : file_(file), line_(line), function_(function) {}
    
    constexpr const char* file_name() const noexcept { return file_; }
    constexpr int line() const noexcept { return line_; }
    constexpr const char* function_name() const noexcept { return function_; }
    
private:
    const char* file_ = "";
    int line_ = 0;
    const char* function_ = "";
};
#endif

// Implementation of boost::throw_exception
void throw_exception(const std::exception& e) {
#ifdef BOOST_NO_EXCEPTIONS
    std::cerr << "Exception: " << e.what() << std::endl;
    std::abort();
#else
    throw e;
#endif
}

// Implementation of boost::throw_exception with source location
void throw_exception(const std::exception& e, const source_location& loc) {
#ifdef BOOST_NO_EXCEPTIONS
    std::cerr << "Exception at " << loc.file_name() << ":" << loc.line() 
              << " in " << loc.function_name() << ": " << e.what() << std::endl;
    std::abort();
#else
    (void)loc; // Suppress unused parameter warning
    throw e;
#endif
}

} // namespace boost
