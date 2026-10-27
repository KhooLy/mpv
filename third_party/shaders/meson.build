shader_files = [
    'anime4k/Anime4K_AutoDownscalePre_x2.glsl',
    'anime4k/Anime4K_AutoDownscalePre_x4.glsl',
    'anime4k/Anime4K_Clamp_Highlights.glsl',
    'anime4k/Anime4K_Deblur_DoG.glsl',
    'anime4k/Anime4K_Deblur_Original.glsl',
    'anime4k/Anime4K_Restore_CNN_M.glsl',
    'anime4k/Anime4K_Restore_CNN_S.glsl',
    'anime4k/Anime4K_Restore_CNN_Soft_M.glsl',
    'anime4k/Anime4K_Restore_CNN_Soft_S.glsl',
    'anime4k/Anime4K_Restore_CNN_Soft_VL.glsl',
    'anime4k/Anime4K_Restore_CNN_VL.glsl',
    'anime4k/Anime4K_Thin_Fast.glsl',
    'anime4k/Anime4K_Thin_HQ.glsl',
    'anime4k/Anime4K_Thin_VeryFast.glsl',
    'anime4k/Anime4K_Upscale_CNN_x2_M.glsl',
    'anime4k/Anime4K_Upscale_CNN_x2_S.glsl',
    'anime4k/Anime4K_Upscale_CNN_x2_VL.glsl',
    'anime4k/Anime4K_Upscale_Denoise_CNN_x2_M.glsl',
    'anime4k/Anime4K_Upscale_Denoise_CNN_x2_S.glsl',
    'anime4k/Anime4K_Upscale_Denoise_CNN_x2_VL.glsl',
    'artcnn/ArtCNN_C4F16_DS.glsl',
    'artcnn/ArtCNN_C4F32_DS.glsl',
]
foreach file: shader_files
    shader = custom_target(file.underscorify(),
        input: file,
        output: file.underscorify() + '.inc',
        command: [file2string, '@INPUT@', '@OUTPUT@', '@SOURCE_ROOT@'],
    )
    sources += shader
endforeach
