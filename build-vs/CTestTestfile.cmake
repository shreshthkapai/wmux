# CMake generated Testfile for 
# Source directory: C:/Users/shres/Desktop/wmux
# Build directory: C:/Users/shres/Desktop/wmux/build-vs
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[wmux.command_parser]=] "C:/Users/shres/Desktop/wmux/build-vs/Debug/wmux_tests.exe")
  set_tests_properties([=[wmux.command_parser]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/shres/Desktop/wmux/CMakeLists.txt;116;add_test;C:/Users/shres/Desktop/wmux/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[wmux.command_parser]=] "C:/Users/shres/Desktop/wmux/build-vs/Release/wmux_tests.exe")
  set_tests_properties([=[wmux.command_parser]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/shres/Desktop/wmux/CMakeLists.txt;116;add_test;C:/Users/shres/Desktop/wmux/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[wmux.command_parser]=] "C:/Users/shres/Desktop/wmux/build-vs/MinSizeRel/wmux_tests.exe")
  set_tests_properties([=[wmux.command_parser]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/shres/Desktop/wmux/CMakeLists.txt;116;add_test;C:/Users/shres/Desktop/wmux/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[wmux.command_parser]=] "C:/Users/shres/Desktop/wmux/build-vs/RelWithDebInfo/wmux_tests.exe")
  set_tests_properties([=[wmux.command_parser]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/shres/Desktop/wmux/CMakeLists.txt;116;add_test;C:/Users/shres/Desktop/wmux/CMakeLists.txt;0;")
else()
  add_test([=[wmux.command_parser]=] NOT_AVAILABLE)
endif()
