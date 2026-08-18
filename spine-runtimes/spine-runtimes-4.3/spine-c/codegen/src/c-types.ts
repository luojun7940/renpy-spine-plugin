import type { ClassOrStruct, DocumentationComment, Enum } from './types';

export interface CParameter {
    name: string;          // Parameter name in C
    cType: string;         // C type (e.g., "float*", "spine_bone")
    cppType: string;       // Original C++ type (e.g., "float&", "Bone*")
    isOutput: boolean;     // true for non-const references that become output params
    isNullable: boolean;   // true for pointer types (can be null), false for references/values
}

export interface CMethod {
    name: string;          // Full C function name (e.g., "spine_bone_get_x")
    returnType: string;    // C return type
    parameters: CParameter[];
    body: string;          // The actual implementation code (e.g., "return ((Bone*)self)->getX();")
    returnTypeNullable: boolean;  // true if return type can be null (pointer types)
    documentation?: DocumentationComment;  // Documentation from C++ source
}

export interface CClassOrStruct {
    name: string;          // C type name (e.g., "spine_bone")
    cppType: ClassOrStruct;  // Original C++ type info
    constructors: CMethod[];  // All constructors (including default)
    destructor: CMethod | null;      // Present unless excluded (calls delete)
    methods: CMethod[];       // All methods
}

export interface CEnumValue {
    name: string;          // C enum value name (e.g., "SPINE_BLEND_MODE_NORMAL")
    value?: string;        // Optional explicit value
}

export interface CEnum {
    name: string;          // C type name (e.g., "spine_blend_mode")
    cppType: Enum;         // Original C++ enum info
    values: CEnumValue[];  // Converted enum values
}