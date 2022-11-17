include(CMakePushCheckState)
include(CheckCSourceCompiles)

macro(CHECK_C_LINKER_FLAG _FLAG _RESULT)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "${_FLAG}")
    check_c_source_compiles("int main() { return 0;}" ${_RESULT})
    cmake_pop_check_state()
endmacro(CHECK_C_LINKER_FLAG)
