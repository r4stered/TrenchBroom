add_library(TbCliLib STATIC)

target_sources(TbCliLib
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Apply.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BuiltRooms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CliError.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CliValidators.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Compile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CsgOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Describe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EditOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorScopes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FaceOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Fgd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FileUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GeometryJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Guide.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IssueTracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IssueTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Leak.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LinkedGroups.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LiveLink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LiveSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LoadMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MapContents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MapRef.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodeJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodePath.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ParseUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Process.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Query.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rooms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Run.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Sha256.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Show.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StructureOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Targets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TextFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformOps.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Verify.cpp

  PUBLIC FILE_SET headers TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(TbCliLib
  PRIVATE
    CompilerConfig
    PrecompileStdHeaders
    TbGlTestUtilsLib
  PUBLIC
    TbMdlLib
    TbQlLib
    TbVersionLib
)

add_subdirectory(test)
