file(GLOB_RECURSE cfiles "./src/*.c") 
file(GLOB_RECURSE cppfiles "./src/*.cpp") 

idf_component_register(SRCS "esp32_nat_router.c" ${cfiles} ${cppfiles}
                    INCLUDE_DIRS "."
                                 "inc")

set_source_files_properties(http_server.c
    PROPERTIES COMPILE_FLAGS
    -Wno-unused-function
)
