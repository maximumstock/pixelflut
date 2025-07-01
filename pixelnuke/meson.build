project('pixelnuke', 'c', version: '0.1', default_options: ['warning_level=3'])


deps = [
	dependency('glfw3'),
	dependency('glew'),
	dependency('libuv'),
  # Looking for both but not requiring either is a hack to make it work on Linux and macOS
  dependency('appleframeworks', modules: 'OpenGL', required: false),
  dependency('opengl', required: false),
]

executable(
	'pixelnuke',
	'pixelnuke.c',
	'canvas.c',
	'net.c',
	install: true,
	dependencies: deps,
)
