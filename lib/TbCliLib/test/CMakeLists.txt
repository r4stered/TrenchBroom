add_executable(TbCliLibTest)
EMBED_UTF8_MANIFEST(TbCliLibTest)

target_sources(TbCliLibTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/CliTestUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Apply.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CliError.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CliValidators.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Config.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_CsgOps.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Describe.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EditOps.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_EntityJson.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FaceOps.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Fgd.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_FileUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_GeometryJson.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_IssueTracker.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_IssueTypes.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Json.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_LoadMap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_MapContents.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NodeJson.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_NodePath.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Ops.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_ParseUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Query.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Run.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Sha256.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Show.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Targets.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TextFormat.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_TransformOps.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tst_Verify.cpp
)

add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS=1)

target_link_libraries(TbCliLibTest
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    Catch2::Catch2WithMain
    TbBaseTestUtilsLib
    TbCliLib
    TbFsTestUtilsLib
)

# Copy fixtures
set(TEST_FIXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixture)
set(TEST_FIXTURE_DEST_DIR $<TARGET_FILE_DIR:TbCliLibTest>/fixture)
set(TB_MDL_TEST_FIXTURE_DIR ${CMAKE_SOURCE_DIR}/lib/TbMdlLib/test/fixture/mdl)

add_custom_command(TARGET TbCliLibTest POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${TEST_FIXTURE_DEST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TEST_FIXTURE_SOURCE_DIR}" "${TEST_FIXTURE_DEST_DIR}/test"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${TB_RESOURCE_DIR}/games/Quake" "${TEST_FIXTURE_DEST_DIR}/games/Quake"
  # a palette and a texture wad, so that tests can tell found textures from missing ones
  COMMAND ${CMAKE_COMMAND} -E copy "${TB_MDL_TEST_FIXTURE_DIR}/Palette/palette.lmp" "${TEST_FIXTURE_DEST_DIR}/textures/palette.lmp"
  COMMAND ${CMAKE_COMMAND} -E copy "${TB_MDL_TEST_FIXTURE_DIR}/LoadMaterialCollections/wads/cr8_a_excerpt.wad" "${TEST_FIXTURE_DEST_DIR}/textures/cr8_a_excerpt.wad"
)

include(Catch)
catch_discover_tests(TbCliLibTest)
