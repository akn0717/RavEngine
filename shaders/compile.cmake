
# debug wireframes shader
declare_shader("debug" "${CMAKE_CURRENT_LIST_DIR}/debug.vsh" "${CMAKE_CURRENT_LIST_DIR}/debug.fsh" "${CMAKE_CURRENT_LIST_DIR}/debug_varying.def.hlsl")

# debug navMesh shader
declare_shader("debugNav" "${CMAKE_CURRENT_LIST_DIR}/debugNav.vsh" "${CMAKE_CURRENT_LIST_DIR}/debugNav.fsh" "${CMAKE_CURRENT_LIST_DIR}/debugNav_varying.def.hlsl")

# deferred geometry material
declare_shader("pbrmaterial" "${CMAKE_CURRENT_LIST_DIR}/deferred_geo.vsh" "${CMAKE_CURRENT_LIST_DIR}/deferred_geo.fsh" "${CMAKE_CURRENT_LIST_DIR}/deferred_varying.def.hlsl")

declare_shader("deferred_blit" "${CMAKE_CURRENT_LIST_DIR}/deferred_blit.vsh" "${CMAKE_CURRENT_LIST_DIR}/deferred_blit.fsh" "${CMAKE_CURRENT_LIST_DIR}/deferred_blit_varying.def.hlsl")

# lighting shaders
declare_shader("pointlightvolume" "${CMAKE_CURRENT_LIST_DIR}/pointlight.vsh" "${CMAKE_CURRENT_LIST_DIR}/pointlight.fsh" "${CMAKE_CURRENT_LIST_DIR}/pointlight_varying.def.hlsl")
declare_shader("ambientlightvolume" "${CMAKE_CURRENT_LIST_DIR}/ambientlight.vsh" "${CMAKE_CURRENT_LIST_DIR}/ambientlight.fsh" "${CMAKE_CURRENT_LIST_DIR}/ambientlight_varying.def.hlsl")
declare_shader("directionallightvolume" "${CMAKE_CURRENT_LIST_DIR}/directionallight.vsh" "${CMAKE_CURRENT_LIST_DIR}/directionallight.fsh" "${CMAKE_CURRENT_LIST_DIR}/directionallight_varying.def.hlsl")
declare_shader("spotlightvolume" "${CMAKE_CURRENT_LIST_DIR}/spotlight.vsh" "${CMAKE_CURRENT_LIST_DIR}/spotlight.fsh" "${CMAKE_CURRENT_LIST_DIR}/spotlight_varying.def.hlsl")

# GUI rendering shaders
declare_shader("guishader" "${CMAKE_CURRENT_LIST_DIR}/gui.vsh" "${CMAKE_CURRENT_LIST_DIR}/gui.fsh" "${CMAKE_CURRENT_LIST_DIR}/gui.def.hlsl")

# skybox
declare_shader("defaultsky" "${CMAKE_CURRENT_LIST_DIR}/defaultsky.vsh" "${CMAKE_CURRENT_LIST_DIR}/defaultsky.fsh" "${CMAKE_CURRENT_LIST_DIR}/defaultsky.def.hlsl")

# debug shader for rendering geometry
declare_shader("meshOnly" "${CMAKE_CURRENT_LIST_DIR}/meshOnlyShader.vsh" "${CMAKE_CURRENT_LIST_DIR}/meshOnlyShader.fsh" "${CMAKE_CURRENT_LIST_DIR}/meshOnlyShader_varying.def.hlsl")

# mesh skinning compute shader
declare_shader("skincompute" "${CMAKE_CURRENT_LIST_DIR}/skinning_cs.glsl" "" "")

# indices copy compute shader
declare_shader("indexcopycompute" "${CMAKE_CURRENT_LIST_DIR}/index_copy_cs.glsl" "" "")

declare_shader("dirlight_pre" "${CMAKE_CURRENT_LIST_DIR}/dirlight_pre.vsh" "${CMAKE_CURRENT_LIST_DIR}/dirlight_pre.fsh" "${CMAKE_CURRENT_LIST_DIR}/dirlight_pre_varying.def.hlsl")

# shadow volume shader
declare_shader("shadowvolume" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShader.vsh" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShader.fsh" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShader_varying.def.hlsl")

# shadow volume - lightweight marking shader
declare_shader("shadowvolumeLT" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShaderLT.vsh" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShaderLT.fsh" "${CMAKE_CURRENT_LIST_DIR}/shadowVolumeShaderLT_varying.def.hlsl")
