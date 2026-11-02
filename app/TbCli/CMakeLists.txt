add_executable(TbCli)
EMBED_UTF8_MANIFEST(TbCli)

set_target_properties(TbCli PROPERTIES OUTPUT_NAME tbcli)

target_sources(TbCli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp
)

target_link_libraries(TbCli
  PRIVATE
    CompilerConfig
    TbCliLib
)
