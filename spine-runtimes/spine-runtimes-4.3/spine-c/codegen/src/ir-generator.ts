import { scanArraySpecializations } from './array-scanner';
import type { CClassOrStruct, CEnum, CEnumValue, CMethod, CParameter } from './c-types';
import { isFieldExcluded, isFieldGetterExcluded, isFieldSetterExcluded } from './exclusions';
import type { ArraySpecialization, Exclusion } from './types';
import { type ClassOrStruct, type Constructor, checkTypeSupport, type Enum, type Field, isPrimitive, isNullable, type Method, type Parameter, type Type, toCFunctionName, toCTypeName, toSnakeCase } from './types';

/**
 * Checks if a type inherits from SpineObject (directly or indirectly)
 */
function inheritsFromSpineObject(type: ClassOrStruct, allTypes: Map<string, Type>): boolean {
    // Check direct inheritance
    if (type.superTypes?.includes('SpineObject')) {
        return true;
    }

    // Check indirect inheritance through all supertypes
    if (type.superTypes) {
        for (const superTypeName of type.superTypes) {
            // Strip template parameters for lookup
            // e.g., "PosedDataGeneric<BoneLocal>" -> "PosedDataGeneric"
            const baseTypeName = superTypeName.split('<')[0].trim();

            const superType = allTypes.get(baseTypeName);
            if (superType && superType.kind !== 'enum') {
                if (inheritsFromSpineObject(superType, allTypes)) {
                    return true;
                }
            }
        }
    }

    return false;
}

export function generateConstructors(type: ClassOrStruct, knownTypeNames: Set<string>, exclusions: Exclusion[], allTypes: Map<string, Type>): CMethod[] {
    const constructors: CMethod[] = [];
    const cTypeName = `spine_${toSnakeCase(type.name)}`;
    const cppTypeName = type.name;

    // Abstract classes cannot be instantiated
    if (type.isAbstract) {
        console.log(`  ${type.name} is abstract - no constructors generated`);
        return constructors;
    }

    // Only generate constructors for classes that inherit from SpineObject
    // (they have the location-based operator new)
    if (!inheritsFromSpineObject(type, allTypes)) {
        console.log(`  ${type.name} does not inherit from SpineObject - no constructors generated`);
        return constructors;
    }

    // Find all constructors (we only have public members in the JSON)
    const ctorMembers = (type.members || [])
        .filter(m => m.kind === 'constructor') as Constructor[];

    // Check if all constructors are excluded
    const allConstructorsExcluded = exclusions.some(e =>
        e.kind === 'method' &&
        e.typeName === type.name &&
        e.methodName === type.name  // Constructor has same name as class
    );

    if (allConstructorsExcluded) {
        // If all constructors are excluded, don't generate any
        console.log(`  Excluding constructor: ${type.name}::${type.name}`);
        return constructors;
    }

    // Only generate constructors that are explicitly defined as public
    // We cannot assume anything about implicit constructors since:
    // 1. For classes, the default constructor is private by default
    // 2. There might be a private/protected default constructor we don't know about
    // The type extractor only shows public members

    // Check if this type has no public constructors
    if (ctorMembers.length === 0) {
        // Check if constructor is explicitly excluded
        const constructorExcluded = exclusions.some(e =>
            e.kind === 'method' &&
            e.typeName === type.name &&
            e.methodName === type.name
        );

        if (constructorExcluded) {
            console.log(`  No public constructors for ${type.name}, but constructor is excluded - type can be used but not created from C`);
        } else {
            console.error(`\nERROR: ${type.name} has no public constructors - it cannot be instantiated from C`);
            console.error(`       You must either:`);
            console.error(`       1. Add a public constructor to the C++ class`);
            console.error(`       2. Add 'type: ${type.name}' to exclusions.txt to exclude the entire type`);
            console.error(`       3. Add 'method: ${type.name}::${type.name}' to exclusions.txt to allow the type but prevent creation`);
            process.exit(1);
        }
    }

    // Generate constructors for each constructor member
    let i = 1;
    for (const ctor of ctorMembers) {
        if (!ctor.parameters || ctor.parameters.length === 0) {
            // Default constructor
            constructors.push({
                name: `${cTypeName}_create`,
                returnType: cTypeName,
                parameters: [],
                body: `return (${cTypeName}) new (__FILE__, __LINE__) ${cppTypeName}();`,
                returnTypeNullable: false,  // Constructors never return null on success
                documentation: ctor.documentation
            });
        } else {
            // Parameterized constructor
            const cParams = convertParameters(ctor.parameters, knownTypeNames);
            const suffix = i === 1 ? "": i;

            // Build the C++ constructor call
            const cppArgs = buildCppArgs(ctor.parameters, cParams, knownTypeNames);

            constructors.push({
                name: `${cTypeName}_create${suffix}`,
                returnType: cTypeName,
                parameters: cParams,
                body: `return (${cTypeName}) new (__FILE__, __LINE__) ${cppTypeName}(${cppArgs});`,
                returnTypeNullable: false,  // Constructors never return null on success
                documentation: ctor.documentation
            });
        }
        i++;
    }

    return constructors;
}


export function generateDestructor(type: ClassOrStruct, exclusions: Exclusion[]): CMethod | null {
    // Check if destructor is excluded
    const isDestructorExcluded = exclusions.some(e =>
        e.kind === 'method' &&
        e.typeName === type.name &&
        e.methodName === `~${type.name}`
    );

    if (isDestructorExcluded) {
        console.log(`  Excluding destructor: ${type.name}::~${type.name}`);
        return null;
    }

    const cTypeName = `spine_${toSnakeCase(type.name)}`;
    const cppTypeName = type.name;

    return {
        name: `${cTypeName}_dispose`,
        returnType: 'void',
        parameters: [{
            name: 'self',
            cType: cTypeName,
            cppType: `${cppTypeName}*`,
            isOutput: false,
            isNullable: false  // Self parameter must never be null (would crash)
        }],
        body: `delete (${cppTypeName}*)self;`,
        returnTypeNullable: false  // void return type cannot be null
    };
}

export function generateMethods(type: ClassOrStruct, knownTypeNames: Set<string>, exclusions: Exclusion[]): CMethod[] {
    const methods: CMethod[] = [];
    const cTypeName = `spine_${toSnakeCase(type.name)}`;
    const cppTypeName = type.name;

    // Get all methods (we only have public members in the JSON)
    const methodMembers = (type.members || [])
        .filter(m => m.kind === 'method') as Method[];

    // Check if this class has getRTTI method
    const hasGetRTTI = methodMembers.some(m => m.name === 'getRTTI');

    // Group methods by name to detect overloads
    const methodsByName = new Map<string, Method[]>();
    for (const method of methodMembers) {
        // Skip excluded methods and operators
        if (isMethodExcluded(type.name, method, exclusions)) {
            console.log(`  Excluding method: ${type.name}::${method.name}${method.isConst ? ' const' : ''}`);
            continue;
        }
        if (method.name.startsWith('operator')) {
            continue;
        }

        const methods = methodsByName.get(method.name) || [];
        methods.push(method);
        methodsByName.set(method.name, methods);
    }

    // Generate methods with appropriate suffixes for overloads
    for (const [methodName, methodOverloads] of methodsByName) {
        // Special handling for methods named "create" to avoid conflicts with constructors
        const isCreateMethod = methodName === 'create';

        if (methodOverloads.length === 1 && !isCreateMethod) {
            // No overloads and not a create method, use standard name
            const cMethod = generateMethod(type, methodOverloads[0], cTypeName, cppTypeName, knownTypeNames);
            if (cMethod) {
                methods.push(cMethod);
            }
        } else {
            // Multiple overloads OR create method, add parameter-based suffix
            let i = 1;
            for (const method of methodOverloads) {
                // For create methods, always add suffix to avoid constructor conflicts
                // For overloads, we need to include type information in the suffix
                const suffix = isCreateMethod ? `method` : i;
                i++;

                const baseCMethodName = toCFunctionName(type.name, method.name);
                const cMethodName = `${baseCMethodName}_${suffix}`;

                const cMethod = generateMethod(type, method, cTypeName, cppTypeName, knownTypeNames, cMethodName);
                if (cMethod) {
                    methods.push(cMethod);
                }
            }
        }
    }

    // If class has getRTTI, also add static rtti method
    if (hasGetRTTI) {
        methods.push({
            name: `${cTypeName}_rtti`,
            returnType: 'spine_rtti',
            parameters: [],
            body: `return (spine_rtti)&${cppTypeName}::rtti;`,
            returnTypeNullable: false  // RTTI method returns a reference address, never null
        });
    }

    return methods;
}

/**
 * Generate getter and setter methods for public fields
 */
export function generateFieldAccessors(type: ClassOrStruct, knownTypeNames: Set<string>, exclusions: Exclusion[]): CMethod[] {
    const methods: CMethod[] = [];
    const cTypeName = `spine_${toSnakeCase(type.name)}`;
    const cppTypeName = type.name;

    // Get all non-static fields (we only have public members in the JSON)
    const fieldMembers = (type.members || [])
        .filter(m => m.kind === 'field' && !m.isStatic) as Field[];

    for (const field of fieldMembers) {
        // Check if entire field is excluded
        if (isFieldExcluded(type.name, field.name, exclusions)) {
            console.log(`  Excluding field: ${type.name}::${field.name}`);
            continue;
        }

        // Check if field type is supported
        const typeError = checkTypeSupport(field.type);
        if (typeError) {
            console.warn(`  Skipping field ${type.name}::${field.name}: ${typeError}`);
            continue;
        }

        const fieldNameSnake = toSnakeCase(field.name);

        // Generate getter
        if (!isFieldGetterExcluded(type.name, field.name, exclusions)) {
            try {
                const cReturnType = toCTypeName(field.type, knownTypeNames);
                const getterName = `${cTypeName}_get_${fieldNameSnake}`;

                methods.push({
                    name: getterName,
                    returnType: cReturnType,
                    parameters: [{
                        name: 'self',
                        cType: cTypeName,
                        cppType: `${cppTypeName}*`,
                        isOutput: false,
                        isNullable: false  // Self parameter must never be null (would crash)
                    }],
                    body: generateFieldGetterBody(field, cppTypeName, knownTypeNames),
                    returnTypeNullable: isNullable(field.type)  // Field return type nullability depends on field type
                });
            } catch (e) {
                console.warn(`  Skipping getter for field ${type.name}::${field.name}: ${e}`);
            }
        } else {
            console.log(`  Excluding field getter: ${type.name}::${field.name}`);
        }

        // Generate setter (only for non-const, non-reference types)
        if (!field.type.includes('const') && !field.type.endsWith('&') && !isFieldSetterExcluded(type.name, field.name, exclusions)) {
            try {
                const cParamType = toCTypeName(field.type, knownTypeNames);
                const setterName = `${cTypeName}_set_${fieldNameSnake}`;

                methods.push({
                    name: setterName,
                    returnType: 'void',
                    parameters: [
                        {
                            name: 'self',
                            cType: cTypeName,
                            cppType: `${cppTypeName}*`,
                            isOutput: false,
                            isNullable: false  // Self parameter must never be null (would crash)
                        },
                        {
                            name: 'value',
                            cType: cParamType,
                            cppType: field.type,
                            isOutput: false,
                            isNullable: isNullable(field.type)  // Value parameter nullability depends on field type
                        }
                    ],
                    body: generateFieldSetterBody(field, cppTypeName, knownTypeNames),
                    returnTypeNullable: false  // void return type cannot be null
                });
            } catch (e) {
                console.warn(`  Skipping setter for field ${type.name}::${field.name}: ${e}`);
            }
        } else if (isFieldSetterExcluded(type.name, field.name, exclusions)) {
            console.log(`  Excluding field setter: ${type.name}::${field.name}`);
        }
    }

    return methods;
}

function generateFieldGetterBody(field: Field, cppTypeName: string, knownTypeNames: Set<string>): string {
    // Use local variable to avoid cast->field line breaks
    const setup = `${cppTypeName} *_self = (${cppTypeName} *) self;`;
    const fieldAccess = `_self->${field.name}`;

    // Handle String fields
    if (field.type === 'String' || field.type === 'const String' || field.type === 'const String&') {
        return `${setup}\n\treturn ${fieldAccess}.buffer();`;
    }

    // Handle reference types
    if (field.type.endsWith('&')) {
        const baseType = field.type.slice(0, -1).trim();
        const cType = toCTypeName(baseType, knownTypeNames);

        if (isPrimitive(baseType)) {
            return `${setup}\n\treturn ${fieldAccess};`;
        }

        return `${setup}\n\treturn (${cType})&${fieldAccess};`;
    }

    // Handle pointer types
    if (field.type.endsWith('*')) {
        const baseType = field.type.slice(0, -1).trim();

        if (isPrimitive(baseType)) {
            return `${setup}\n\treturn ${fieldAccess};`;
        }

        const cType = toCTypeName(field.type, knownTypeNames);
        return `${setup}\n\treturn (${cType})${fieldAccess};`;
    }

    // Handle enum types
    if (knownTypeNames.has(field.type)) {
        const cType = toCTypeName(field.type, knownTypeNames);
        return `${setup}\n\treturn (${cType})${fieldAccess};`;
    }

    // Handle primitive types
    if (isPrimitive(field.type)) {
        return `${setup}\n\treturn ${fieldAccess};`;
    }

    // Handle non-primitive value types (need to return address)
    const cType = toCTypeName(field.type, knownTypeNames);
    return `${setup}\n\treturn (${cType})&${fieldAccess};`;
}

function generateFieldSetterBody(field: Field, cppTypeName: string, knownTypeNames: Set<string>): string {
    // Use local variable to avoid cast->field line breaks
    const setup = `${cppTypeName} *_self = (${cppTypeName} *) self;`;
    const fieldAccess = `_self->${field.name}`;

    // Handle String fields
    if (field.type === 'String') {
        return `${setup}\n\t${fieldAccess} = String(value);`;
    }

    // Handle Array types
    if (field.type.startsWith('Array<')) {
        const arrayMatch = field.type.match(/^Array<(.+?)>$/);
        if (arrayMatch) {
            const elementType = arrayMatch[1];
            return `${setup}\n\t${fieldAccess} = *((Array<${elementType}>*)value);`;
        }
    }

    // Handle enum types
    if (knownTypeNames.has(field.type)) {
        return `${setup}\n\t${fieldAccess} = (${field.type})value;`;
    }

    // Handle pointer types (cast back from opaque type)
    if (field.type.endsWith('*') && !isPrimitive(field.type)) {
        const baseType = field.type.slice(0, -1).trim();
        return `${setup}\n\t${fieldAccess} = (${baseType}*)value;`;
    }

    // Handle everything else
    return `${setup}\n\t${fieldAccess} = value;`;
}

/**
 * Generate CClassOrStruct for an array specialization
 */
export function generateArrayType(spec: ArraySpecialization, arrayType: ClassOrStruct, knownTypeNames: Set<string>): CClassOrStruct {
    const cTypeName = spec.cTypeName;

    // Generate array constructors
    const constructors = [
        {
            name: `${cTypeName}_create`,
            returnType: cTypeName,
            parameters: [],
            body: `return (${cTypeName}) new (__FILE__, __LINE__) Array<${spec.elementType}>();`,
            returnTypeNullable: false  // Array constructors never return null on success
        },
        {
            name: `${cTypeName}_create_with_capacity`,
            returnType: cTypeName,
            parameters: [{
                name: 'initialCapacity',
                cType: 'size_t',
                cppType: 'size_t',
                isOutput: false,
                isNullable: false  // size_t cannot be null
            }],
            body: `return (${cTypeName}) new (__FILE__, __LINE__) Array<${spec.elementType}>(initialCapacity);`,
            returnTypeNullable: false  // Array constructors never return null on success
        }
    ];

    // Generate destructor
    const destructor = {
        name: `${cTypeName}_dispose`,
        returnType: 'void',
        parameters: [{
            name: 'array',
            cType: cTypeName,
            cppType: `Array<${spec.elementType}>*`,
            isOutput: false,
            isNullable: false  // Array parameter must never be null (would crash)
        }],
        body: `delete (Array<${spec.elementType}>*)array;`,
        returnTypeNullable: false  // void return type cannot be null
    };

    // Generate array methods
    const methods = generateArrayMethods(spec.elementType, cTypeName, arrayType, knownTypeNames);

    return {
        name: cTypeName,
        cppType: arrayType,
        constructors,
        destructor,
        methods
    };
}

export function generateArrayMethods(elementType: string, cTypeName: string, arrayType: ClassOrStruct, knownTypeNames: Set<string>): CMethod[] {
    const methods: CMethod[] = [];
    const cppElementType = elementType;
    const cElementType = toCTypeName(elementType, knownTypeNames);

    // Generate array-specific methods
    const arrayMethods = (arrayType.members || []).filter(m => m.kind === 'method') as Method[];

    for (const method of arrayMethods) {
        // Skip constructors/destructors (handled separately)
        if (method.name === 'Array' || method.name === '~Array') {
            continue;
        }

        // Skip operator overloads
        if (method.name.startsWith('operator')) {
            continue;
        }

        // Replace template parameter T with actual element type
        const specializedMethod = specializeArrayMethod(method, cppElementType);

        const cMethod = generateArrayMethod(cTypeName, specializedMethod, cppElementType, cElementType, knownTypeNames);
        if (cMethod) {
            methods.push(cMethod);
        }
    }

    return methods;
}

export function generateEnum(enumType: Enum): CEnum {
    const cEnumName = `spine_${toSnakeCase(enumType.name)}`;
    const enumNameUpper = toSnakeCase(enumType.name).toUpperCase();
    const values: CEnumValue[] = [];

    for (const value of enumType.values) {
        // Convert value name to snake case
        let valueNameSnake = toSnakeCase(value.name).toUpperCase();

        // Remove redundant prefix if the value name starts with the enum name
        // e.g., for TextureFilter::TextureFilter_Nearest, remove the "TextureFilter_" prefix
        const enumNameCamelCase = enumType.name.toUpperCase();
        if (value.name.toUpperCase().startsWith(enumNameCamelCase)) {
            let withoutPrefix = value.name.slice(enumType.name.length);
            // Also strip leading underscore if present
            if (withoutPrefix.startsWith('_')) {
                withoutPrefix = withoutPrefix.slice(1);
            }
            // Only use the stripped version if it's not empty
            if (withoutPrefix.length > 0) {
                valueNameSnake = toSnakeCase(withoutPrefix).toUpperCase();
            }
        }

        values.push({
            name: `SPINE_${enumNameUpper}_${valueNameSnake}`,
            value: value.value
        });
    }

    return {
        name: cEnumName,
        cppType: enumType,
        values
    };
}

// Helper functions

function convertParameters(params: Parameter[], knownTypeNames: Set<string>): CParameter[] {
    const cParams: CParameter[] = [];

    for (const param of params) {
        const cType = toCTypeName(param.type, knownTypeNames);
        const isOutput = isOutputParameter(param.type);
        const paramIsNullable = isNullable(param.type);

        cParams.push({
            name: param.name,
            cType,
            cppType: param.type,
            isOutput,
            isNullable: paramIsNullable
        });
    }

    return cParams;
}

function isOutputParameter(cppType: string): boolean {
    // Non-const references to primitives become output parameters
    const refMatch = cppType.match(/^((?:const\s+)?(.+?))\s*&$/);
    if (refMatch) {
        const fullBaseType = refMatch[1].trim();
        const baseType = refMatch[2].trim();
        const isConst = fullBaseType.startsWith('const ');

        return !isConst && isPrimitive(baseType);
    }

    return false;
}

/**
 * Converts a C parameter to its C++ argument form for function calls.
 * Handles type conversions, casts, and dereferencing as needed.
 */
function convertArgumentToCpp(cppType: string, cParamName: string, isOutput: boolean, knownTypeNames: Set<string>): string {
    // Handle String parameters
    if (cppType === 'const String&' || cppType === 'String') {
        return `String(${cParamName})`;
    }

    // Handle const String* parameters
    if (cppType === 'const String*') {
        return `(const String*)${cParamName}`;
    }

    // Handle pointer parameters (need to cast from opaque type)
    if (cppType.endsWith('*')) {
        const baseType = cppType.slice(0, -1).trim();
        if (!isPrimitive(baseType)) {
            // Cast from opaque C type to C++ type
            return `(${cppType})${cParamName}`;
        } else {
            return cParamName;
        }
    }

    // Handle reference parameters
    if (cppType.endsWith('&')) {
        const baseType = cppType.slice(0, -1).trim();

        // Non-const primitive refs are output parameters
        if (isOutput) {
            return `*${cParamName}`;
        }

        // For T const& where T is a pointer type (e.g., Animation* const&)
        if (baseType.endsWith(' const')) {
            // Remove const to get the actual type
            const actualType = baseType.slice(0, -6).trim(); // Remove ' const'
            if (actualType.endsWith('*')) {
                // It's a pointer, just cast and use directly
                return `(${actualType})${cParamName}`;
            }
        }

        // Handle String& and const String& in reference context
        if (baseType === 'String' || baseType === 'const String') {
            return `String(${cParamName})`;
        }

        // Class references need to dereference the pointer
        if (!isPrimitive(baseType)) {
            return `*((${baseType}*)${cParamName})`;
        }

        // Const primitive refs
        return cParamName;
    }

    // Handle enum parameters
    if (cppType.match(/^(?:const\s+)?([A-Z]\w+)(?:\s*&)?$/) && knownTypeNames.has(cppType.replace(/^const\s+/, '').replace(/\s*&$/, ''))) {
        const enumType = cppType.replace(/^const\s+/, '').replace(/\s*&$/, '');
        return `(${enumType})${cParamName}`;
    }

    // Handle everything else
    return cParamName;
}

function buildCppArgs(cppParams: Parameter[], cParams: CParameter[], knownTypeNames: Set<string>): string {
    const args: string[] = [];

    for (let i = 0; i < cppParams.length; i++) {
        const cppParam = cppParams[i];
        const cParam = cParams[i];
        args.push(convertArgumentToCpp(cppParam.type, cParam.name, cParam.isOutput, knownTypeNames));
    }

    return args.join(', ');
}

function generateMethod(type: ClassOrStruct, method: Method, cTypeName: string, cppTypeName: string, knownTypeNames: Set<string>, customMethodName?: string): CMethod | null {
    try {
        const cReturnType = toCTypeName(method.returnType, knownTypeNames);
        const cMethodName = customMethodName || toCFunctionName(type.name, method.name);

        // Convert parameters
        const cParams: CParameter[] = [];

        // Add self parameter for non-static methods
        if (!method.isStatic) {
            cParams.push({
                name: 'self',
                cType: cTypeName,
                cppType: `${cppTypeName}*`,
                isOutput: false,
                isNullable: false  // Self parameter must never be null (would crash)
            });
        }

        // Add method parameters
        if (method.parameters) {
            cParams.push(...convertParameters(method.parameters, knownTypeNames));
        }

        // Generate method body
        let methodCall: string;
        let body: string;

        if (method.isStatic) {
            methodCall = `${cppTypeName}::${method.name}(${buildCppArgs(method.parameters || [], cParams, knownTypeNames)})`;
            body = generateReturnStatement(method.returnType, methodCall, knownTypeNames);
        } else {
            const instanceVar = `${cppTypeName} *_self = (${cppTypeName} *) self;`;
            methodCall = `_self->${method.name}(${buildCppArgs(method.parameters || [], cParams.slice(1), knownTypeNames)})`;
            const returnStatement = generateReturnStatement(method.returnType, methodCall, knownTypeNames);
            body = `${instanceVar}\n\t${returnStatement}`;
        }

        return {
            name: cMethodName,
            returnType: cReturnType,
            parameters: cParams,
            body,
            returnTypeNullable: isNullable(method.returnType),
            documentation: method.documentation
        };
    } catch (e) {
        console.warn(`Skipping method ${type.name}::${method.name}: ${e}`);
        return null;
    }
}

/**
 * Generates the return statement for a method based on its return type.
 * Handles type conversions, casts, and special cases like String and references.
 */
function generateReturnStatement(returnType: string, methodCall: string, knownTypeNames: Set<string>,
                                  elementType?: string, cElementType?: string): string {
    const isVoid = returnType === 'void';

    // Handle void returns
    if (isVoid) {
        return `${methodCall};`;
    }

    // Handle String returns (with or without space before &)
    if (returnType === 'String' || returnType === 'const String') {
        // error!
        throw new Error(`String return type not supported: ${returnType}`);
    }

    if(returnType === 'const String&' || returnType === 'const String &') {
        return `return ${methodCall}.buffer();`;
    }

    // Handle element type returns (for array methods)
    if (elementType && returnType === elementType) {
        if (elementType.endsWith('*') && !isPrimitive(elementType)) {
            return `return (${cElementType})${methodCall};`;
        }
        return `return ${methodCall};`;
    }

    // Handle reference returns
    if (returnType.endsWith('&')) {
        const baseType = returnType.slice(0, -1).trim();

        // Handle element type references (for array methods)
        if (elementType && returnType === `${elementType}&`) {
            if (elementType.endsWith('*') && !isPrimitive(elementType)) {
                return `return (${cElementType})&${methodCall};`;
            }
            return `return &${methodCall};`;
        }

        const cType = toCTypeName(baseType, knownTypeNames);

        // For primitive references, return the value
        if (isPrimitive(baseType)) {
            return `return ${methodCall};`;
        }

        // For class references, return the address cast to opaque type
        return `return (${cType})&${methodCall};`;
    }

    // Handle pointer returns
    if (returnType.endsWith('*')) {
        const baseType = returnType.slice(0, -1).trim();

        // Primitive pointers pass through
        if (isPrimitive(baseType)) {
            return `return ${methodCall};`;
        }

        // Class pointers need cast
        const cType = toCTypeName(returnType, knownTypeNames);
        return `return (${cType})${methodCall};`;
    }

    // Handle enum returns
    if (knownTypeNames.has(returnType)) {
        const cType = toCTypeName(returnType, knownTypeNames);
        return `return (${cType})${methodCall};`;
    }

    // Handle primitive returns
    return `return ${methodCall};`;
}

function specializeArrayMethod(method: Method, cppElementType: string): Method {
    // Helper to replace T with the actual element type, handling const correctly
    const replaceT = (type: string): string => {
        // For const T&, if T is a pointer, we need T const& not const T&
        if (type === 'const T &' && cppElementType.endsWith('*')) {
            return `${cppElementType} const &`;
        }
        // For const T*, if T is a pointer, keep it as is
        if (type === 'const T *' && cppElementType.endsWith('*')) {
            return `${cppElementType} const *`;
        }
        // Otherwise do simple replacement
        return type.replace(/\bT\b/g, cppElementType);
    };

    const specializedReturnType = replaceT(method.returnType);
    const specializedParams = method.parameters?.map(p => ({
        ...p,
        type: replaceT(p.type)
    }));

    return {
        ...method,
        returnType: specializedReturnType,
        parameters: specializedParams
    };
}

function generateArrayMethod(cTypeName: string, method: Method, cppElementType: string, cElementType: string, knownTypeNames: Set<string>): CMethod | null {
    try {
        const cReturnType = toCTypeName(method.returnType, knownTypeNames);
        const cMethodName = `${cTypeName}_${toSnakeCase(method.name)}`;

        // Convert parameters
        const cParams: CParameter[] = [];

        // Add self parameter
        cParams.push({
            name: 'array',
            cType: cTypeName,
            cppType: `Array<${cppElementType}>*`,
            isOutput: false,
            isNullable: false  // Array parameter must never be null (would crash)
        });

        // Add method parameters
        if (method.parameters) {
            cParams.push(...convertParameters(method.parameters, knownTypeNames));
        }

        // Generate method body for array
        const body = generateArrayMethodBody(method, cppElementType, cElementType, knownTypeNames);

        return {
            name: cMethodName,
            returnType: cReturnType,
            parameters: cParams,
            body,
            returnTypeNullable: isNullable(method.returnType),
            documentation: method.documentation
        };
    } catch (e) {
        console.warn(`Skipping array method ${method.name}: ${e}`);
        return null;
    }
}

function generateArrayMethodBody(method: Method, cppElementType: string, cElementType: string, knownTypeNames: Set<string>): string {
    // Use local variable to avoid cast->method line breaks
    const setup = `Array<${cppElementType}> *_array = (Array<${cppElementType}> *) array;`;

    // Build method call arguments using shared function
    const cppArgs = method.parameters ?
        buildCppArgs(method.parameters, convertParameters(method.parameters, knownTypeNames), knownTypeNames) :
        '';

    const methodCall = `_array->${method.name}(${cppArgs})`;

    // Use shared return value handling
    const returnStatement = generateReturnStatement(method.returnType, methodCall, knownTypeNames, cppElementType, cElementType);
    return `${setup}\n\t${returnStatement}`;
}

function isMethodExcluded(typeName: string, method: Method, exclusions: Exclusion[]): boolean {
    for (const exclusion of exclusions) {
        if (exclusion.kind === 'method' &&
            exclusion.typeName === typeName &&
            exclusion.methodName === method.name) {
            // If isConst is specified, it must match
            if (exclusion.isConst !== undefined) {
                return exclusion.isConst === method.isConst;
            }
            // Otherwise, exclude all overloads
            return true;
        }
    }
    return false;
}

export async function generateTypes(types: Type[], exclusions: Exclusion[], allExtractedTypes: Type[]): Promise<{ cTypes: CClassOrStruct[], cEnums: CEnum[] }> {
    const knownTypeNames = new Set<string>(types.map(t => t.name));
    const allTypesMap = new Map<string, Type>(allExtractedTypes.map(t => [t.name, t]));

    // Generate C intermediate representation for classes/structs
    const cTypes: CClassOrStruct[] = [];
    for (const type of types.filter(t => t.kind !== 'enum')) {
        console.log(`Generating ${type.name}...`);

        // Generate IR
        const constructors = generateConstructors(type, knownTypeNames, exclusions, allTypesMap);
        const destructor = generateDestructor(type, exclusions);
        const methods = generateMethods(type, knownTypeNames, exclusions);
        const fieldAccessors = generateFieldAccessors(type, knownTypeNames, exclusions);

        // Combine all methods
        const allMethods = [...methods, ...fieldAccessors];

        const cType: CClassOrStruct = {
            name: `spine_${toSnakeCase(type.name)}`,
            cppType: type,
            constructors,
            destructor,
            methods: allMethods
        };

        cTypes.push(cType);
    }

    // Generate C intermediate representation for enums
    const cEnums: CEnum[] = [];
    for (const enumType of types.filter(t => t.kind === 'enum')) {
        console.log(`Generating enum ${enumType.name}...`);

        const cEnum = generateEnum(enumType);
        cEnums.push(cEnum);
    }

    return { cTypes, cEnums };
}

export function generateArrays(types: Type[], arrayType: ClassOrStruct, exclusions: Exclusion[]): CClassOrStruct[] {
    const knownTypeNames = new Set<string>(types.map(t => t.name));

    // Generate Array specializations
    console.log('\nScanning for Array specializations...');
    const arraySpecializations = scanArraySpecializations(types, exclusions);
    console.log(`Found ${arraySpecializations.length} array specializations to generate`);

    // Generate array types
    const cArrayTypes: CClassOrStruct[] = [];
    for (const arraySpecialization of arraySpecializations) {
        const arrayIR = generateArrayType(arraySpecialization, arrayType, knownTypeNames);
        cArrayTypes.push(arrayIR);
    }
    return cArrayTypes;
}