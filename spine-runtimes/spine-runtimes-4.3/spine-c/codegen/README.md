# Spine C API Code Generator

This TypeScript-based code generator automatically creates a C wrapper API for the Spine C++ runtime. It parses the spine-cpp headers using Clang's AST and generates a complete C API with opaque types, following systematic type conversion rules. The generator also builds inheritance maps and interface information for multi-language binding generation.

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Type System](#type-system)
4. [File Structure](#file-structure)
5. [Usage](#usage)
6. [Type Conversion Rules](#type-conversion-rules)
7. [Exclusions System](#exclusions-system)
8. [Validation Checks](#validation-checks)
9. [Array Specializations](#array-specializations)
10. [Generated Code Examples](#generated-code-examples)
11. [Implementation Details](#implementation-details)
12. [Development Tools](#development-tools)
13. [Troubleshooting](#troubleshooting)

## Overview

The code generator performs static analysis on the spine-cpp headers to automatically generate a C API that wraps the C++ classes. It handles:

- Type conversions between C++ and C
- Method wrapping with proper parameter marshaling
- Memory management through constructors and destructors
- Enum conversions
- Array specializations for different element types
- Field accessors (getters/setters) for public fields
- Automatic validation and conflict detection
- Inheritance analysis and interface detection for multi-language bindings

## Architecture

The generator follows a multi-stage pipeline:

1. **Type Extraction** (`type-extractor.ts`)
   - Uses Clang's `-ast-dump=json` to parse C++ headers
   - Extracts all public members (methods, fields, constructors, destructors)
   - Handles template types and inheritance relationships
   - Outputs to `spine-cpp-types.json`

2. **Type Processing** (`index.ts`)
   - Loads extracted types and exclusions
   - Filters out template types, excluded types, and internal classes
   - Determines which types inherit from SpineObject
   - Validates that all types meet generation requirements

3. **Validation** (`checks.ts`)
   - Detects const/non-const method conflicts
   - Identifies multi-level pointers
   - Finds field accessor conflicts
   - Checks for method/type name collisions
   - Validates return types

4. **Array Scanning** (`array-scanner.ts`)
   - Scans all types for `Array<T>` usage
   - Generates specialized array types for each element type
   - Handles primitive, pointer, and enum arrays

5. **IR Generation** (`ir-generator.ts`)
   - Converts C++ types to C intermediate representation
   - Generates wrapper methods with proper marshaling
   - Creates field accessors for public fields
   - Adds constructors and destructors

6. **Code Writing** (`c-writer.ts`)
   - Writes header files with C function declarations
   - Writes implementation files with C++ wrapper code
   - Generates array specialization files
   - Creates main include files (`types.h`)

7. **Inheritance Analysis**
   - Builds inheritance maps for single-inheritance languages (Dart, Swift, Java)
   - Identifies pure interfaces vs concrete classes
   - Detects multiple concrete inheritance (not supported)
   - Generates inheritance information for language binding generators

## Type System

### Type Categories

The generator categorizes types for systematic conversion:

1. **Primitives**: Direct mapping (int, float, bool, size_t, etc.)
2. **Special Types**: Custom conversions (String → const char*, PropertyId → int64_t)
3. **Arrays**: Template specializations (Array<T> → spine_array_<type>)
4. **Pointers**: Class pointers become opaque types
5. **References**: Converted based on const-ness and type
6. **Enums**: Prefixed and snake_cased
7. **Classes**: Converted to opaque pointers with prefix

### C++ to C Type Mapping

The generator uses opaque pointers for all C++ classes:
- `Skeleton*` → `spine_skeleton` (opaque pointer)
- `const Skeleton*` → `const spine_skeleton`
- `Skeleton&` → `spine_skeleton` (references become pointers)

### Special Types
- `String` → `const char*`
- `PropertyId` → `int64_t`
- `Array<T>` → `spine_array_T` (specialized types)

### Primitive Types
- Primitives pass through unchanged: `int`, `float`, `bool`, etc.
- Non-const primitive references become pointers: `float&` → `float*`

## File Structure

```
codegen/
├── src/
│   ├── index.ts           # Main entry point and orchestration
│   ├── type-extractor.ts  # Clang AST parsing
│   ├── cpp-check.ts       # C++ nullability analysis tool
│   ├── types.ts           # Type definitions and conversion logic
│   ├── c-types.ts         # C IR type definitions
│   ├── array-scanner.ts   # Array specialization detection
│   ├── checks.ts          # Validation checks
│   ├── exclusions.ts      # Exclusion handling
│   ├── ir-generator.ts    # C++ to C IR conversion
│   ├── c-writer.ts        # File generation
│   └── warnings.ts        # Warning collection
├── dist/                  # TypeScript compilation output
├── exclusions.txt         # Type/method exclusions
├── spine-cpp-types.json   # Extracted type information
├── nullable.md            # C++ nullability analysis results
├── out.json              # Debug output file
├── package.json           # Node.js configuration
├── tsconfig.json          # TypeScript configuration
├── tsfmt.json            # TypeScript formatter configuration
├── biome.json            # Biome linter configuration
└── node_modules/         # Dependencies
```

Generated files are output to `../src/generated/`:
- Individual files per type (e.g., `skeleton.h`, `skeleton.cpp`)
- `types.h` - Forward declarations for all types
- `arrays.h/cpp` - Array specializations

## Usage

```bash
# Install dependencies
npm install

# Run the code generator
npx tsx src/index.ts

# Or export JSON for debugging
npx tsx src/index.ts --export-json

# The generated files will be in ../src/generated/
```

### C++ Nullability Analysis Tool

The codegen includes a tool to analyze spine-cpp for nullability patterns:

```bash
# Generate nullable.md with clickable links to methods with nullable inputs/outputs
npm run cpp-check
```

This tool identifies all methods that either:
- Return pointer types (nullable return values)
- Take pointer parameters (nullable inputs)

The output `nullable.md` contains clickable markdown links for easy navigation in VS Code. This is useful for cleaning up the spine-cpp API to use references vs pointers appropriately to signal nullability.

The generator automatically:
- Detects when spine-cpp headers have changed
- Regenerates only when necessary
- Reports warnings and errors during generation
- Formats the generated C++ code using the project's formatter
- Builds inheritance maps for multi-language binding generation

## Type Conversion Rules

### Primitive Types
Primitive types are "pass-through".

```
C++ Type          → C Type
─────────────────────────────────
int               → int
float*            → float*
const char*       → const char*
bool              → bool (stdbool.h)
size_t            → size_t
```

### Class Types
```
C++ Type          → C Type
─────────────────────────────────
Bone*             → spine_bone
const Bone*       → const spine_bone
Bone&             → spine_bone
const Bone&       → spine_bone
```

### Special Cases
```
C++ Type          → C Type
─────────────────────────────────
String            → const char*
String&           → const char*
const String&     → const char*
PropertyId        → int64_t
Array<float>      → spine_array_float
Array<Bone*>      → spine_array_bone
```

### Output Parameters
```
C++ Type          → C Type
─────────────────────────────────
float&            → float* (output param)
int&              → int* (output param)
```

### Function Naming
```
C++ Method                → C Function
─────────────────────────────────────────
Skeleton::updateCache()   → spine_skeleton_update_cache()
AnimationState::apply()   → spine_animation_state_apply()
Bone::getX()              → spine_bone_get_x()
```

## Exclusions System

The `exclusions.txt` file controls what gets generated:

### Type Exclusions
Exclude entire types from generation:
```
type: SkeletonClipping
type: Triangulator
```

### Method Exclusions
Exclude specific methods:
```
method: AnimationState::setListener
method: AnimationState::addListener
```

### Const-Specific Exclusions
Exclude only const or non-const versions:
```
method: BoneData::getSetupPose const
```

### Constructor Exclusions
Allow type but prevent instantiation:
```
method: AtlasRegion::AtlasRegion
```

### Field Accessor Exclusions
Control field getter/setter generation:
```
field: AtlasRegion::names          # Exclude both getter and setter
field-get: SecretData::password    # Exclude only getter
field-set: Bone::x                 # Exclude only setter
```

### Type-Wide Field Exclusions
Exclude all field accessors for a type:
```
field: RenderCommand               # No field accessors at all
field-get: DebugData               # No getters (write-only fields)
field-set: RenderCommand           # No setters (read-only fields)
```

## Validation Checks

The generator performs extensive validation to ensure correctness:

### 1. Const/Non-Const Conflicts
Detects methods with both const and non-const versions:
```cpp
T& getValue();              // Non-const version
const T& getValue() const;  // Const version
```

### 2. Multi-Level Pointers
Rejects types with multiple pointer levels:
```cpp
char** strings;  // Not supported
void*** ptr;     // Not supported
```

### 3. Field Accessor Conflicts
Detects when generated accessors would conflict with existing methods:
```cpp
class Bone {
    float x;           // Would generate get_x/set_x
    float getX();      // Conflicts with generated getter
};
```

### 4. Method/Type Name Conflicts
Ensures generated function names don't collide with type names:
```cpp
class BonePose { };     // → spine_bone_pose
class Bone {
    void pose();        // → spine_bone_pose (conflict!)
};
```

### 5. Value Return Types
Detects methods returning non-primitive types by value:
```cpp
Color getColor();  // Cannot return objects by value in C
```

## Array Specializations

The generator automatically creates specialized array types for any `Array<T>` found in the API:

### Primitive Arrays
```cpp
Array<float>      → spine_array_float
Array<int>        → spine_array_int
Array<unsigned short> → spine_array_unsigned_short
```

### Pointer Arrays
```cpp
Array<Bone*>      → spine_array_bone
Array<Slot*>      → spine_array_slot
Array<float*>     → spine_array_float_ptr
```

### Enum Arrays
```cpp
Array<BlendMode>  → spine_array_blend_mode
Array<PropertyId> → spine_array_property_id
```

### Unsupported Arrays
- `Array<String>` - Use `const char**` instead
- `Array<Array<T>>` - Nested arrays not supported
- Arrays with const elements

## Generated Code Examples

### Class Wrapper
```c
// Header: skeleton.h
typedef struct spine_skeleton* spine_skeleton;

spine_skeleton spine_skeleton_create(spine_skeleton_data data);
void spine_skeleton_dispose(spine_skeleton self);
void spine_skeleton_update_cache(spine_skeleton self);
float spine_skeleton_get_x(const spine_skeleton self);
void spine_skeleton_set_x(spine_skeleton self, float value);

// Implementation: skeleton.cpp
spine_skeleton spine_skeleton_create(spine_skeleton_data data) {
    return (spine_skeleton) new (__FILE__, __LINE__) Skeleton((SkeletonData*)data);
}

void spine_skeleton_update_cache(spine_skeleton self) {
    ((Skeleton*)self)->updateCache();
}
```

### Enum Wrapper
```c
// Header: blend_mode.h
#ifndef SPINE_SPINE_BLEND_MODE_H
#define SPINE_SPINE_BLEND_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum spine_blend_mode {
    SPINE_BLEND_MODE_NORMAL = 0,
    SPINE_BLEND_MODE_ADDITIVE,
    SPINE_BLEND_MODE_MULTIPLY,
    SPINE_BLEND_MODE_SCREEN
} spine_blend_mode;

#ifdef __cplusplus
}
#endif

#endif /* SPINE_SPINE_BLEND_MODE_H */
```

### Array Specialization
Arrays are generated as opaque types with complete CRUD operations. All arrays are consolidated into `arrays.h` and `arrays.cpp`.

```c
// Header: arrays.h
SPINE_OPAQUE_TYPE(spine_array_float)

// Creation functions
spine_array_float spine_array_float_create(void);
spine_array_float spine_array_float_create_with_capacity(size_t initialCapacity);

// Memory management
void spine_array_float_dispose(spine_array_float array);
void spine_array_float_clear(spine_array_float array);

// Size and capacity operations
size_t spine_array_float_get_capacity(spine_array_float array);
size_t spine_array_float_size(spine_array_float array);
spine_array_float spine_array_float_set_size(spine_array_float array, size_t newSize, float defaultValue);
void spine_array_float_ensure_capacity(spine_array_float array, size_t newCapacity);

// Element operations
void spine_array_float_add(spine_array_float array, float inValue);
void spine_array_float_add_all(spine_array_float array, spine_array_float inValue);
void spine_array_float_clear_and_add_all(spine_array_float array, spine_array_float inValue);
void spine_array_float_remove_at(spine_array_float array, size_t inIndex);

// Search operations
bool spine_array_float_contains(spine_array_float array, float inValue);
int spine_array_float_index_of(spine_array_float array, float inValue);

// Direct buffer access
float *spine_array_float_buffer(spine_array_float array);

// Implementation: arrays.cpp
spine_array_float spine_array_float_create(void) {
    return (spine_array_float) new (__FILE__, __LINE__) Array<float>();
}

void spine_array_float_dispose(spine_array_float array) {
    delete (Array<float> *) array;
}

void spine_array_float_add(spine_array_float array, float inValue) {
    Array<float> *_array = (Array<float> *) array;
    _array->add(inValue);
}

float *spine_array_float_buffer(spine_array_float array) {
    Array<float> *_array = (Array<float> *) array;
    return _array->buffer();
}
```

Arrays are generated for all basic types (`float`, `int`, `unsigned_short`, `property_id`) and all object types used in collections throughout the API. The implementation directly casts the opaque handle to the underlying `Array<T>*` type.

## Implementation Details

### Memory Management
- All C++ objects inheriting from `SpineObject` use location-based `operator new`
- Constructors use `new (__FILE__, __LINE__)` for memory tracking
- Destructors call `delete` on the C++ object
- Array types are wrapped in structs to maintain C++ semantics

### Constructor Generation
- Only generates constructors for non-abstract classes
- Only generates constructors for classes inheriting from `SpineObject`
- Requires at least one public constructor or explicit exclusion
- Constructor overloads are numbered: `_create`, `_create2`, `_create3`

### Field Accessor Generation
- Generates getters for all non-static public fields
- Generates setters for non-const, non-reference fields
- Uses direct field access, not C++ getter/setter methods
- Handles nested field access (e.g., `obj.field.x`)

### Method Overloading
- Constructor overloads are numbered: `_create`, `_create2`, `_create3`, etc.
- Method overloads are numbered with suffixes: `_1`, `_2`, `_3`, etc.
- Methods named "create" get `_method` suffix to avoid constructor conflicts
- Const/non-const conflicts are detected and reported

### RTTI Handling
- Uses Spine's custom RTTI system (`getRTTI().instanceOf()`)
- No C++ RTTI or exceptions are used
- RTTI checks are performed in generated code where needed

### Warning System
- Collects non-fatal issues during generation using `WarningsCollector`
- Reports abstract classes, missing constructors, etc.
- Groups warnings by pattern to avoid repetition
- Warnings don't stop generation but are reported at the end

### Interface Detection
- Automatically identifies pure interfaces (classes with only pure virtual methods)
- Distinguishes between concrete classes and interfaces for inheritance mapping
- Used to determine extends vs implements relationships for target languages

### Multiple Inheritance Handling
- Detects multiple concrete inheritance scenarios
- Fails generation with clear error messages when unsupported patterns are found
- Provides guidance on converting concrete classes to interfaces

## Troubleshooting

### Common Errors

1. **"Unknown type: X"**
   - The type is not a primitive and not in the extracted types
   - Solution: Add to exclusions or check spelling

2. **"Multi-level pointers are not supported"**
   - Type contains `**` or more pointers
   - Solution: Refactor C++ code or exclude

3. **"Array<String> is not supported"**
   - String arrays need special handling
   - Solution: Use `const char**` in C++ or exclude

4. **"No public constructors"**
   - Class has no public constructors for instantiation
   - Solution: Add public constructor or exclude

5. **"Method/type name conflict"**
   - Generated function name collides with a type name
   - Solution: Rename method or exclude

6. **"Multiple concrete inheritance detected"**
   - A class inherits from multiple concrete (non-interface) classes
   - Solution: Convert one of the parent classes to a pure interface
   - Check the error message for specific guidance on which classes to modify

### Debugging Tips

1. Check `spine-cpp-types.json` for extracted type information
2. Look for "Excluding" messages in console output
3. Verify inheritance with "inherits from SpineObject" messages
4. Array specializations are listed with element type mapping
5. Check warnings at the end of generation for issues
6. Use `--export-json` flag to export inheritance and type information as JSON
7. Check `out.json` for debug output when troubleshooting
8. Review console output for inheritance mapping information (extends/mixins)

### Adding New Types

1. Ensure the type is in spine-cpp headers
2. Remove from exclusions.txt if previously excluded
3. Check that all dependent types are included
4. Run generator and fix any reported issues
5. Verify generated code compiles

### Performance Considerations

- Type extraction uses Clang AST parsing (slow but accurate)
- File generation is parallelized where possible
- Array scanning happens after type filtering for efficiency
- Validation checks run before generation to fail fast
- Incremental generation avoids regenerating unchanged files

## Development Tools

The codegen project includes several development tools and configurations:

### Biome Configuration (`biome.json`)
- Linting enabled with recommended rules
- Formatting disabled (uses external formatter)
- Helps maintain code quality during development

### TypeScript Formatter (`tsfmt.json`)
- Comprehensive formatting rules for TypeScript code
- Configures indentation, spacing, and code style
- Used for consistent code formatting across the project

### Build Output (`dist/`)
- Contains compiled TypeScript files
- Generated JavaScript and declaration files
- Source maps for debugging

### Debug Output (`out.json`)
- Contains debug information from the generation process
- Useful for troubleshooting and understanding the generated data structure

### Dependencies
The project uses minimal dependencies for maximum compatibility:
- `@types/node` - Node.js type definitions
- `tsx` - TypeScript execution engine
- `typescript-formatter` - Code formatting
- `@biomejs/biome` - Fast linter for code quality