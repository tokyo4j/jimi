glslang = find_program('glslangValidator')
shaders = []
foreach shader : [
  'texture.vert',
  'texture.frag',
  'box.vert',
  'solid.frag',
  'shadow.frag',
]
  shaders += custom_target(
    shader + '.h',
    input: shader,
    output: shader + '.h',
    command: [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@',
      '--vn', shader.underscorify() + '_spv'])
endforeach

jimi_sources += shaders
