# spine-c

The spine-c runtime provides basic functionality to load and manipulate [Spine](http://esotericsoftware.com) skeletal animation data using C. It contains a generic `SkeletonRenderer` that returns render commands that can be easily fed into any rendering API that supports textured triangle meshes with blend modes. See [spine-glfw](../spine-glfw), [spine-sdl](../spine-sdl), or [spine-sfml](../spine-sfml) for examples.

**Note:** spine-c is a C wrapper around [spine-cpp](../spine-cpp) for use in environments that cannot easily interact with C++ code. The spine-c code is generated using a code generator. For details on the code generation process, please see the [codegen/README.md](codegen/README.md).

# See the [spine-c documentation](http://esotericsoftware.com/spine-c) for in-depth information

## Licensing

You are welcome to evaluate the Spine Runtimes and the examples we provide in this repository free of charge.

You can integrate the Spine Runtimes into your software free of charge, but users of your software must have their own [Spine license](https://esotericsoftware.com/spine-purchase). Please make your users aware of this requirement! This option is often chosen by those making development tools, such as an SDK, game toolkit, or software library.

In order to distribute your software containing the Spine Runtimes to others that don't have a Spine license, you need a [Spine license](https://esotericsoftware.com/spine-purchase) at the time of integration. Then you can distribute your software containing the Spine Runtimes however you like, provided others don't modify it or use it to create new software. If others want to do that, they'll need their own Spine license.

For the official legal terms governing the Spine Runtimes, please read the [Spine Runtimes License Agreement](http://esotericsoftware.com/spine-runtimes-license) and Section 2 of the [Spine Editor License Agreement](http://esotericsoftware.com/spine-editor-license#s2).

## Spine version

spine-c works with data exported from Spine 4.3.xx.

spine-c supports all Spine features.

## Usage

### Integration with CMake (Recommended)

The easiest way to integrate spine-c into your project is via CMake FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(
    spine-c
    GIT_REPOSITORY https://github.com/esotericsoftware/spine-runtimes.git
    GIT_TAG 4.3
    SOURCE_SUBDIR spine-c
)
FetchContent_MakeAvailable(spine-c)

# Link against spine-c
target_link_libraries(your_target spine-c)
```

This will automatically fetch and build spine-c along with its dependency (spine-cpp).

### Manual Integration

If you prefer manual integration:

1. Download the Spine Runtimes source using git (`git clone https://github.com/esotericsoftware/spine-runtimes`) or download it as a zip.
2. Add the required source files to your project:
   - Add sources from `spine-cpp/src`, `spine-c/src`
3. Add the include directories: `spine-cpp/include`, `spine-c/include`

See the [Spine Runtimes documentation](http://esotericsoftware.com/spine-documentation#runtimes) for detailed API usage.

## Runtimes extending spine-c

- [spine-ios](../spine-ios)
- [spine-flutter](../spine-flutter)
- [spine-sdl](../spine-sdl)
- [spine-glfw](../spine-glfw)