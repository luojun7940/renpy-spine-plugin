import { isMethodExcluded } from "./exclusions";
import { type ClassOrStruct, type Exclusion, type Field, isPrimitive, type Method, type Type, toSnakeCase } from "./types";
import type { CClassOrStruct, CMethod } from "./c-types";

/**
 * Checks for methods that have both const and non-const versions with different return types.
 * This is a problem for C bindings because C doesn't support function overloading.
 *
 * In C++, you can have:
 *   T& getValue();              // for non-const objects
 *   const T& getValue() const;  // for const objects
 *
 * But in C, we can only have one function with a given name, so we need to detect
 * and report these conflicts.
 *
 * @param classes - Array of class/struct types to check
 * @param exclusions - Exclusion rules to skip specific methods
 * @returns Array of conflicts found, or exits if conflicts exist
 */
export function checkConstNonConstConflicts(classes: ClassOrStruct[], exclusions: Exclusion[]): void {
    const conflicts: Array<{ type: string, method: string }> = [];

    for (const type of classes) {
        if (type.members === undefined) {
            continue;
        }

        // Get all non-static methods
        const allMethods = type.members?.filter(m =>
            m.kind === 'method' &&
            !m.isStatic
        ) as Method[] | undefined;

        if (allMethods) {
            const methodGroups = new Map<string, Method[]>();
            for (const method of allMethods) {
                // Skip if this specific const/non-const version is excluded
                if (isMethodExcluded(type.name, method.name, exclusions, { isConst: method.isConst })) {
                    continue;
                }
                const key = method.name + '(' + (method.parameters?.map(p => p.type).join(',') || '') + ')';
                if (!methodGroups.has(key)) {
                    methodGroups.set(key, []);
                }
                methodGroups.get(key)!.push(method as Method);
            }

            for (const [signature, group] of methodGroups) {
                if (group.length > 1) {
                    // Check if we have both const and non-const versions
                    const hasConst = group.some(m => m.isConst === true);
                    const hasNonConst = group.some(m => m.isConst === false);

                    if (hasConst && hasNonConst) {
                        conflicts.push({ type: type.name, method: group[0].name });
                    }
                }
            }
        }
    }

    // If we found conflicts, report them all and exit
    if (conflicts.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("SUMMARY OF ALL CONST/NON-CONST METHOD CONFLICTS");
        console.error("=".repeat(80));
        console.error(`\nFound ${conflicts.length} method conflicts across the codebase:\n`);

        for (const conflict of conflicts) {
            console.error(`  - ${conflict.type}::${conflict.method}()`);
        }

        console.error("\nThese methods have both const and non-const versions in C++ which cannot");
        console.error("be represented in the C API. You need to either:");
        console.error("  1. Add these to exclusions.txt");
        console.error("  2. Modify the C++ code to avoid const/non-const overloading");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}

/**
 * Checks for multi-level pointers in method signatures and errors if found
 */
export function checkMultiLevelPointers(types: ClassOrStruct[]) {
    const errors: { type: string, member: string, signature: string }[] = [];

    // Helper to check if a type has multi-level pointers
    function hasMultiLevelPointers(typeStr: string): boolean {
        // First check the outer type (after removing template content)
        let outerType = typeStr;

        // Extract all template contents for separate checking
        const templateContents: string[] = [];
        let depth = 0;
        let templateStart = -1;

        for (let i = 0; i < typeStr.length; i++) {
            if (typeStr[i] === '<') {
                if (depth === 0) {
                    templateStart = i + 1;
                }
                depth++;
            } else if (typeStr[i] === '>') {
                depth--;
                if (depth === 0 && templateStart !== -1) {
                    templateContents.push(typeStr.substring(templateStart, i));
                    templateStart = -1;
                }
            }
        }

        // Remove all template content from outer type
        outerType = outerType.replace(/<[^>]+>/g, '<>');

        // Check outer type for consecutive pointers
        if (/\*\s*\*/.test(outerType)) {
            return true;
        }

        // Recursively check template contents
        for (const content of templateContents) {
            if (hasMultiLevelPointers(content)) {
                return true;
            }
        }

        return false;
    }

    for (const type of types) {
        if (!type.members) continue;

        for (const member of type.members) {
            if (member.kind === 'method') {
                // Check return type
                if (hasMultiLevelPointers(member.returnType)) {
                    errors.push({
                        type: type.name,
                        member: member.name,
                        signature: `return type: ${member.returnType}`
                    });
                }

                // Check parameters
                if (member.parameters) {
                    for (const param of member.parameters) {
                        if (hasMultiLevelPointers(param.type)) {
                            errors.push({
                                type: type.name,
                                member: member.name,
                                signature: `parameter '${param.name}': ${param.type}`
                            });
                        }
                    }
                }
            } else if (member.kind === 'field') {
                // Check field type
                if (hasMultiLevelPointers(member.type)) {
                    errors.push({
                        type: type.name,
                        member: member.name,
                        signature: `field type: ${member.type}`
                    });
                }
            }
        }
    }

    // If we found multi-level pointers, report them and exit
    if (errors.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("MULTI-LEVEL POINTER ERROR");
        console.error("=".repeat(80));
        console.error(`\nFound ${errors.length} multi-level pointer usage(s) which are not supported:\n`);

        for (const error of errors) {
            console.error(`  - ${error.type}::${error.member} - ${error.signature}`);
        }

        console.error("\nMulti-level pointers (e.g., char**, void***) cannot be represented in the C API.");
        console.error("You need to either:");
        console.error("  1. Refactor the C++ code to avoid multi-level pointers");
        console.error("  2. Exclude these types/methods in exclusions.txt");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}

/**
 * Checks for conflicts between generated field accessors and existing methods.
 * For example, if a class has a public field 'x' and also has methods getX() or setX(),
 * the generated getters/setters would conflict.
 */
export function checkFieldAccessorConflicts(classes: ClassOrStruct[], exclusions: Exclusion[]): void {
    const conflicts: Array<{ type: string, field: string, conflictingMethod: string, accessorType: 'getter' | 'setter' }> = [];

    for (const type of classes) {
        if (!type.members) continue;

        // Get all non-static fields
        const fields = type.members.filter(m =>
            m.kind === 'field' &&
            !m.isStatic
        ) as Field[];

        // Get all methods
        const methods = type.members.filter(m =>
            m.kind === 'method'
        ) as Method[];

        // For each field, check if getter/setter would conflict
        for (const field of fields) {
            const fieldNameSnake = toSnakeCase(field.name);
            const getterName = `get_${fieldNameSnake}`;
            const setterName = `set_${fieldNameSnake}`;

            // Check for getter conflicts
            for (const method of methods) {
                if (isMethodExcluded(type.name, method.name, exclusions, { isConst: method.isConst })) {
                    continue;
                }

                const methodNameSnake = toSnakeCase(method.name);

                // Check if this method would conflict with the generated getter
                if (methodNameSnake === getterName || methodNameSnake === `get${field.name.toLowerCase()}`) {
                    conflicts.push({
                        type: type.name,
                        field: field.name,
                        conflictingMethod: method.name,
                        accessorType: 'getter'
                    });
                }

                // Check if this method would conflict with the generated setter
                if (!field.type.includes('const') && !field.type.endsWith('&')) {
                    if (methodNameSnake === setterName || methodNameSnake === `set${field.name.toLowerCase()}`) {
                        conflicts.push({
                            type: type.name,
                            field: field.name,
                            conflictingMethod: method.name,
                            accessorType: 'setter'
                        });
                    }
                }
            }
        }
    }

    // If we found conflicts, report them
    if (conflicts.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("FIELD ACCESSOR CONFLICTS");
        console.error("=".repeat(80));
        console.error(`\nFound ${conflicts.length} conflicts between public fields and existing methods:\n`);

        for (const conflict of conflicts) {
            console.error(`  - ${conflict.type}::${conflict.field} would generate ${conflict.accessorType} that conflicts with ${conflict.type}::${conflict.conflictingMethod}()`);
        }

        console.error("\nThese fields have corresponding getter/setter methods in C++.");
        console.error("You should either:");
        console.error("  1. Make the field private/protected in C++");
        console.error("  2. Exclude the conflicting method in exclusions.txt");
        console.error("  3. Add field exclusion support to skip generating accessors for specific fields");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}

/**
 * Checks for method names that would conflict with type names when converted to C.
 * For example, if we have a type BonePose (→ spine_bone_pose) and a method Bone::pose() (→ spine_bone_pose)
 */
export function checkMethodTypeNameConflicts(classes: ClassOrStruct[], allTypes: Type[], exclusions: Exclusion[]): void {
    const conflicts: Array<{ className: string, methodName: string, conflictingType: string }> = [];

    // Build a set of all C type names
    const cTypeNames = new Set<string>();
    for (const type of allTypes) {
        cTypeNames.add(`spine_${toSnakeCase(type.name)}`);
    }

    // Check all methods
    for (const type of classes) {
        if (!type.members) continue;

        const methods = type.members!.filter(m =>
            m.kind === 'method'
        ) as Method[];

        for (const method of methods) {
            // Skip excluded methods
            if (isMethodExcluded(type.name, method.name, exclusions, method)) {
                continue;
            }

            // Generate the C function name
            const cFunctionName = `spine_${toSnakeCase(type.name)}_${toSnakeCase(method.name)}`;

            // Check if this conflicts with any type name
            if (cTypeNames.has(cFunctionName)) {
                // Find which type it conflicts with
                const conflictingType = allTypes.find(t =>
                    `spine_${toSnakeCase(t.name)}` === cFunctionName
                );

                if (conflictingType) {
                    conflicts.push({
                        className: type.name,
                        methodName: method.name,
                        conflictingType: conflictingType!.name
                    });
                }
            }
        }
    }

    // Report conflicts
    if (conflicts.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("METHOD/TYPE NAME CONFLICTS");
        console.error("=".repeat(80));
        console.error(`\nFound ${conflicts.length} method names that conflict with type names:\n`);

        for (const conflict of conflicts) {
            console.error(`  - ${conflict.className}::${conflict.methodName}() conflicts with type ${conflict.conflictingType}`);
            console.error(`    Both generate the same C name: spine_${toSnakeCase(conflict.className)}_${toSnakeCase(conflict.methodName)}`);
        }

        console.error("\nThese conflicts cannot be resolved automatically. You must either:");
        console.error("  1. Rename the method or type in C++");
        console.error("  2. Exclude the method in exclusions.txt");
        console.error("  3. Exclude the conflicting type in exclusions.txt");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}

/**
 * Checks for getter/setter pairs where the return type nullability of the getter
 * doesn't match the parameter nullability of the setter. This creates inconsistent
 * APIs in target languages (e.g., Dart) where such mismatches are forbidden.
 * 
 * @param cTypes - Array of generated C types with their methods
 */
export function checkGetterSetterNullabilityMismatch(cTypes: CClassOrStruct[]): void {
    const mismatches: Array<{ 
        typeName: string, 
        fieldName: string, 
        getterNullable: boolean, 
        setterNullable: boolean 
    }> = [];

    for (const cType of cTypes) {
        if (!cType.methods) continue;

        // Group methods by field name (extract from method names like spine_type_get_field, spine_type_set_field)
        const fieldAccessors = new Map<string, { getter?: CMethod, setter?: CMethod }>();

        for (const method of cType.methods) {
            // Check if this is a getter method (ends with _get_<field_name> AND has exactly 1 parameter)
            const getterMatch = method.name.match(/^(.+)_get_(.+)$/);
            if (getterMatch && method.parameters?.length === 1) {
                const fieldName = getterMatch[2];
                if (!fieldAccessors.has(fieldName)) {
                    fieldAccessors.set(fieldName, {});
                }
                fieldAccessors.get(fieldName)!.getter = method;
                continue;
            }

            // Check if this is a setter method (ends with _set_<field_name> AND has exactly 2 parameters)
            const setterMatch = method.name.match(/^(.+)_set_(.+)$/);
            if (setterMatch && method.parameters?.length === 2) {
                const fieldName = setterMatch[2];
                if (!fieldAccessors.has(fieldName)) {
                    fieldAccessors.set(fieldName, {});
                }
                fieldAccessors.get(fieldName)!.setter = method;
            }
        }

        // Check each getter/setter pair for nullability mismatches
        for (const [fieldName, accessors] of fieldAccessors) {
            const { getter, setter } = accessors;

            // Skip if we don't have both getter and setter
            if (!getter || !setter) continue;

            // Extract nullability information
            const getterNullable = getter.returnTypeNullable || false;
            
            // For setters, find the parameter that's not 'self' (should be the value parameter)
            const valueParam = setter.parameters?.find(p => p.name !== 'self');
            if (!valueParam) continue;
            
            const setterNullable = valueParam.isNullable || false;

            // Check for mismatch
            if (getterNullable !== setterNullable) {
                mismatches.push({
                    typeName: cType.name,
                    fieldName,
                    getterNullable,
                    setterNullable
                });
            }
        }
    }

    // Report mismatches
    if (mismatches.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("GETTER/SETTER NULLABILITY MISMATCHES");
        console.error("=".repeat(80));
        console.error(`\nFound ${mismatches.length} getter/setter pairs with mismatched nullability:\n`);

        for (const mismatch of mismatches) {
            const getterType = mismatch.getterNullable ? "nullable" : "non-nullable";
            const setterType = mismatch.setterNullable ? "nullable" : "non-nullable";
            console.error(`  - ${mismatch.typeName}::${mismatch.fieldName}`);
            console.error(`    Getter returns: ${getterType}`);  
            console.error(`    Setter expects: ${setterType}`);
        }

        console.error("\nThese nullability mismatches cause compilation errors in some target");
        console.error("languages (e.g., Dart). The getter and setter must have consistent nullability.");
        console.error("You should either:");
        console.error("  1. Ensure the C++ field type has consistent nullability semantics");
        console.error("  2. Exclude problematic field getters or setters in exclusions.txt");
        console.error("  3. Override nullability analysis for specific field types");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}

/**
 * Checks for methods that return non-primitive types by value.
 * These cannot be wrapped in C without heap allocation.
 */
export function checkValueReturns(classes: ClassOrStruct[], allTypes: Type[], exclusions: Exclusion[]): void {
    const issues: Array<{ type: string, method: string, returnType: string }> = [];

    // Build a set of enum type names for quick lookup
    const enumTypes = new Set<string>();
    for (const type of allTypes) {
        if (type.kind === 'enum') {
            enumTypes.add(type.name);
        }
    }

    for (const type of classes) {
        if (!type.members) continue;

        const methods = type.members.filter(m =>
            m.kind === 'method'
        ) as Method[];

        for (const method of methods) {
            // Skip excluded methods
            if (isMethodExcluded(type.name, method.name, exclusions, method)) {
                continue;
            }

            const returnType = method.returnType;

            // Skip void, primitives, pointers, and references
            if (returnType === 'void' ||
                isPrimitive(returnType) ||
                returnType.endsWith('*') ||
                returnType.endsWith('&')) {
                continue;
            }

            // Skip String (handled specially)
            if (returnType === 'String' || returnType === 'const String') {
                continue;
            }

            // Skip enums (they're just integers in C)
            if (enumTypes.has(returnType)) {
                continue;
            }

            // This is a non-primitive type returned by value
            issues.push({
                type: type.name,
                method: method.name,
                returnType: returnType
            });
        }
    }

    // Report issues
    if (issues.length > 0) {
        console.error("\n" + "=".repeat(80));
        console.error("METHODS RETURNING OBJECTS BY VALUE");
        console.error("=".repeat(80));
        console.error(`\nFound ${issues.length} methods that return non-primitive types by value:\n`);

        for (const issue of issues) {
            console.error(`  - ${issue.type}::${issue.method}() returns ${issue.returnType}`);
        }

        console.error("\nC cannot return objects by value through opaque pointers.");
        console.error("You must either:");
        console.error("  1. Change the C++ method to return a pointer or reference");
        console.error("  2. Exclude the method in exclusions.txt");
        console.error("=".repeat(80) + "\n");

        process.exit(1);
    }
}