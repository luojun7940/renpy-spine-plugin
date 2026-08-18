export interface DocumentationComment {
    summary?: string;          // Main description
    details?: string;          // Extended description
    params?: { [name: string]: string };  // Parameter descriptions
    returns?: string;          // Return value description
    deprecated?: string;       // Deprecation notice
    since?: string;           // Version info
    see?: string[];           // Related references
}

export interface Parameter {
    name: string;
    type: string;
}

export type Field = {
    kind: 'field';
    name: string;
    type: string;
    isStatic?: boolean;
    fromSupertype?: string;
    documentation?: DocumentationComment;
    loc: {
        line: number;
        col: number;
    };
}

export type Method = {
    kind: 'method';
    name: string;
    returnType: string;
    parameters?: Parameter[];
    isStatic?: boolean;
    isVirtual?: boolean;
    isPure?: boolean;
    isConst?: boolean;
    fromSupertype?: string;
    documentation?: DocumentationComment;
    loc: {
        line: number;
        col: number;
    };
}

export type Constructor = {
    kind: 'constructor';
    name: string;
    parameters?: Parameter[];
    fromSupertype?: string;
    documentation?: DocumentationComment;
    loc: {
        line: number;
        col: number;
    };
}

export type Destructor = {
    kind: 'destructor';
    name: string;
    isVirtual?: boolean;
    isPure?: boolean;
    fromSupertype?: string;
    documentation?: DocumentationComment;
    loc: {
        line: number;
        col: number;
    };
};

export type Member =
    | Field
    | Method
    | Constructor
    | Destructor

export type Enum = {
    kind: 'enum';
    name: string;
    values: EnumValue[];
    documentation?: DocumentationComment;
    loc: {
        line: number;
        col: number;
    };
}

export interface EnumValue {
    name: string;
    value?: string;
}

export interface ClassOrStruct {
    name: string;
    kind: 'class' | 'struct';
    loc: {
        line: number;
        col: number;
    };
    superTypes?: string[];
    members?: Member[];
    isAbstract?: boolean;
    isTemplate?: boolean;
    templateParams?: string[];
    documentation?: DocumentationComment;
}

export type Type = ClassOrStruct | Enum;

export type Exclusion =
    | {
        kind: 'type';
        typeName: string;
    }
    | {
        kind: 'method';
        typeName: string;
        methodName: string;
        isConst?: boolean;  // Whether the method is const (e.g., void foo() const), NOT whether return type is const
    }
    | {
        kind: 'field';
        typeName: string;
        fieldName: string;
    }
    | {
        kind: 'field-get';
        typeName: string;
        fieldName: string;
    }
    | {
        kind: 'field-set';
        typeName: string;
        fieldName: string;
    };

export interface ArraySpecialization {
    cppType: string;           // e.g. "Array<float>"
    elementType: string;       // e.g. "float"
    cTypeName: string;         // e.g. "spine_array_float"
    cElementType: string;      // e.g. "float" or "spine_animation"
    isPointer: boolean;
    isEnum: boolean;
    isPrimitive: boolean;
    sourceMember: Member;
}

/**
 * Converts a PascalCase or camelCase name to snake_case.
 *
 * @param name The name to convert
 * @returns The snake_case version
 *
 * Examples:
 * - "AnimationState" → "animation_state"
 * - "getRTTI" → "get_rtti"
 * - "IKConstraint" → "ik_constraint"
 */
export function toSnakeCase(name: string): string {
    // Handle acronyms and consecutive capitals
    let result = name.replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2');
    // Insert underscore before capital letters
    result = result.replace(/([a-z\d])([A-Z])/g, '$1_$2');
    return result.toLowerCase();
}

/**
 * Generates a C function name from a type and method name.
 *
 * @param typeName The C++ class name
 * @param methodName The method name
 * @returns The C function name
 *
 * Examples:
 * - ("Skeleton", "updateCache") → "spine_skeleton_update_cache"
 * - ("AnimationState", "apply") → "spine_animation_state_apply"
 */
export function toCFunctionName(typeName: string, methodName: string): string {
    return `spine_${toSnakeCase(typeName)}_${toSnakeCase(methodName)}`;
}

/**
 * Checks if a type is a primitive by tokenizing and checking if ALL tokens start with lowercase.
 * Examples:
 * - "int" → true
 * - "const char*" → true (all tokens: "const", "char*" start lowercase)
 * - "unsigned int" → true (all tokens start lowercase)
 * - "Array<float>" → false (starts uppercase)
 * - "const Array<float>&" → false ("Array" starts uppercase)
 */
export function isPrimitive(cppType: string): boolean {
    const tokens = cppType.split(/\s+/);
    return tokens.every(token => {
        // Remove any trailing punctuation like *, &
        const cleanToken = token.replace(/[*&]+$/, '');
        return cleanToken.length > 0 && /^[a-z]/.test(cleanToken);
    });
}

/**
 * Converts a C++ type to its corresponding C type.
 *
 * @param cppType The C++ type to convert
 * @param knownTypeNames Set of known type names (classes and enums) from filtered types
 * @returns The C type
 * @throws Error if the type is not recognized
 *
 * Examples:
 * - Primitives: "int" → "int", "const float*" → "const float*"
 * - String types: "String" → "const char*", "const String&" → "const char*"
 * - Arrays: "Array<float>" → "spine_array_float"
 * - Class pointers: "Bone*" → "spine_bone"
 * - Class references: "const Color&" → "spine_color"
 * - Non-const primitive refs: "float&" → "float*" (output parameter)
 */
export function toCTypeName(cppType: string, knownTypeNames: Set<string>): string {
    // Remove extra spaces and normalize
    const normalizedType = cppType.replace(/\s+/g, ' ').trim();

    // Primitives - pass through unchanged
    if (isPrimitive(normalizedType)) {
        return normalizedType;
    }

    // Special type: String
    if (normalizedType === 'String' || normalizedType === 'const String' ||
        normalizedType === 'String&' || normalizedType === 'const String&') {
        return 'const char *';
    }

    // Special Type: PropertyId is a typedef
    if (normalizedType === 'PropertyId') {
        return 'int64_t';
    }

    // Arrays - must check before pointers/references
    const arrayMatch = normalizedType.match(/^(?:const\s+)?Array<(.+?)>\s*(?:&|\*)?$/);
    if (arrayMatch) {
        const elementType = arrayMatch[1].trim();

        // For primitive element types, use the type name with spaces replaced by underscores
        if (isPrimitive(elementType)) {
            return `spine_array_${elementType.replace(/\s+/g, '_')}`;
        }

        // For pointer types, remove the * and convert
        if (elementType.endsWith('*')) {
            const baseType = elementType.slice(0, -1).trim();
            return `spine_array_${toSnakeCase(baseType)}`;
        }

        // For class/enum types
        return `spine_array_${toSnakeCase(elementType)}`;
    }

    // Handle "Type * const" pattern (const pointer)
    if (normalizedType.endsWith(' const')) {
        const baseType = normalizedType.slice(0, -6).trim(); // Remove ' const'
        return toCTypeName(baseType, knownTypeNames);
    }

    // Pointers
    const pointerMatch = normalizedType.match(/^(.+?)\s*\*$/);
    if (pointerMatch) {
        const baseType = pointerMatch[1].trim();

        // Primitive pointers stay as-is, but ensure space before *
        if (isPrimitive(baseType)) {
            return `${baseType} *`;
        }

        // Speical case for PropertyId
        if (baseType === 'PropertyId') {
            return 'int64_t *'; // PropertyId is a typedef for int64_t
        }

        // Class pointers become opaque types
        return `spine_${toSnakeCase(baseType)}`;
    }

    // References
    const refMatch = normalizedType.match(/^((?:const\s+)?(.+?))\s*&$/);
    if (refMatch) {
        const fullBaseType = refMatch[1].trim();
        const baseType = refMatch[2].trim();
        const isConst = fullBaseType.startsWith('const ');

        // Non-const references to primitives become pointers (output parameters)
        if (!isConst && isPrimitive(baseType)) {
            return `${baseType} *`;
        }

        // Const references and class references - recurse without the reference
        return toCTypeName(baseType, knownTypeNames);
    }

    // Function pointers - for now, just error
    if (normalizedType.includes('(') && normalizedType.includes(')')) {
        throw new Error(`Function pointer types not yet supported: ${normalizedType}`);
    }

    // Handle const types - strip const and recurse
    if (normalizedType.startsWith('const ') && !normalizedType.includes('*') && !normalizedType.includes('&')) {
        const baseType = normalizedType.slice(6).trim(); // Remove 'const '
        return toCTypeName(baseType, knownTypeNames);
    }

    // Everything else should be a class or enum type
    // Check if it's a valid type
    if (!knownTypeNames.has(normalizedType)) {
        throw new Error(`Unknown type: ${normalizedType}. Not a primitive and not in the list of valid types.`);
    }

    return `spine_${toSnakeCase(normalizedType)}`;
}

/**
 * Determines if a C++ type represents a nullable value (pointer types).
 * Based on spine-cpp convention: pointers are nullable, references are not.
 *
 * @param cppType The C++ type to check
 * @returns true if the type can be null (pointer types), false otherwise
 *
 * Examples:
 * - "Bone*" → true (pointer, can be null)
 * - "const Bone*" → true (pointer, can be null)
 * - "Bone&" → false (reference, cannot be null)
 * - "const Bone&" → false (reference, cannot be null)
 * - "int" → false (value type, cannot be null)
 * - "float*" → true (pointer to primitive, can be null)
 */
export function isNullable(cppType: string): boolean {
    const normalizedType = cppType.replace(/\s+/g, ' ').trim();
    
    // Reference types are NOT nullable (even if they contain pointers inside templates)
    if (normalizedType.includes('&')) {
        return false;
    }
    
    // Pointer types are nullable (only if not a reference)
    if (normalizedType.includes('*')) {
        return true;
    }
    
    // Value types and primitives are NOT nullable
    return false;
}

/**
 * Checks if a C++ type can be represented in the C API.
 * Returns null if the type can be handled, or an error message if not.
 */
export function checkTypeSupport(cppType: string): string | null {
    // Remove extra spaces and normalize
    const normalizedType = cppType.replace(/\s+/g, ' ').trim();

    // Check for Array<String> which we can't handle
    const arrayMatch = normalizedType.match(/^(?:const\s+)?Array<(.+?)>\s*(?:&|\*)?$/);
    if (arrayMatch) {
        const elementType = arrayMatch[1].trim();
        if (elementType === 'String') {
            return "Array<String> is not supported - use const char** instead";
        }
    }

    // Check for multi-level pointers
    if (/\*\s*\*/.test(normalizedType)) {
        return "Multi-level pointers are not supported";
    }

    // Check for function pointers
    if (normalizedType.includes('(') && normalizedType.includes(')')) {
        return "Function pointer types are not supported";
    }

    return null;
}