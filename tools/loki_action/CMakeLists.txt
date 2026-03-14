set(TARGET loki_action)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

set(TARGET_SRCS
    loki_action_lib.cpp
)

add_library(${TARGET} SHARED ${TARGET_SRCS})

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib
)

target_include_directories(${TARGET} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(${TARGET} PRIVATE cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

target_compile_features(${TARGET} PRIVATE cxx_std_17)

if (ANDROID)
    find_library(log-lib log)
    target_link_libraries(${TARGET} PRIVATE ${log-lib})
endif()

if (UNIX AND NOT APPLE)
    set_target_properties(${TARGET} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_compile_options(${TARGET} PRIVATE -fvisibility=hidden)
    target_compile_definitions(${TARGET} PRIVATE LOKI_ACTION_API_EXPORTS)
endif()

if (WIN32)
    target_compile_definitions(${TARGET} PRIVATE LOKI_ACTION_API_EXPORTS)
endif()

install(TARGETS ${TARGET} LIBRARY DESTINATION lib)
install(FILES loki_action_api.h DESTINATION include)
