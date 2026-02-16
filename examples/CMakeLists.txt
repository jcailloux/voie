function(voie_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE voie)
endfunction()

voie_add_example(hello)
voie_add_example(rest_api)
