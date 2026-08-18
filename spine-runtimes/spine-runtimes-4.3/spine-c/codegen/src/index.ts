#!/usr/bin/env node
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CWriter } from './c-writer';
import { checkConstNonConstConflicts, checkFieldAccessorConflicts, checkGetterSetterNullabilityMismatch, checkMethodTypeNameConflicts, checkMultiLevelPointers, checkValueReturns } from './checks';
import { isTypeExcluded, loadExclusions } from './exclusions';
import { generateArrays, generateTypes } from './ir-generator';
import { extractTypes } from './type-extractor';
import type { ClassOrStruct, Method } from './types';
import { toSnakeCase } from './types';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export async function generate() {
    // Load all exclusions
    const exclusions = loadExclusions(path.join(__dirname, '../exclusions.txt'));

    // Extract ALL types from spine-cpp first (needed for inheritance checking)
    const allExtractedTypes = extractTypes();
    allExtractedTypes.sort((a, b) => a.name.localeCompare(b.name));

    // Then filter out excluded and template types
    let arrayType: ClassOrStruct | undefined;
    const types = allExtractedTypes.filter(type => {
        // Store the Array type, needed for array specializations
        if (type.name === 'Array' && type.kind !== 'enum') {
            arrayType = type as ClassOrStruct;
        }

        if (isTypeExcluded(type.name, exclusions)) {
            console.log(`Excluding type due to exclusions.txt: ${type.name}`);
            return false;
        }

        // Only exclude template types
        if (type.kind !== 'enum' && type.isTemplate === true) {
            console.log(`Auto-excluding template type: ${type.name}`);
            return false;
        }

        // Include everything else (including abstract types)
        return true;
    });

    if (!arrayType) {
        throw new Error('Array type not found in types');
    }

    // Separate classes and enums
    const classes = types.filter(t => t.kind !== "enum");
    const enums = types.filter(t => t.kind === 'enum');

    console.log(`Found ${classes.length} classes/structs and ${enums.length} enums to generate`);

    // Check for const/non-const conflicts
    checkConstNonConstConflicts(classes, exclusions);

    // Check for multi-level pointers
    checkMultiLevelPointers(classes);

    // Check for field accessor conflicts
    checkFieldAccessorConflicts(classes, exclusions);

    // Check for method/type name conflicts
    checkMethodTypeNameConflicts(classes, allExtractedTypes, exclusions);

    // Check for methods returning objects by value
    checkValueReturns(classes, allExtractedTypes, exclusions);

    // Generate C intermediate representation for classes, enums and arrays
    const { cTypes, cEnums } = await generateTypes(types, exclusions, allExtractedTypes);
    const cArrayTypes = await generateArrays(types, arrayType, exclusions);

    // Check for getter/setter nullability mismatches
    checkGetterSetterNullabilityMismatch(cTypes);

    // Build interface/pure type information first
    const isInterface = buildInterfaceMap(allExtractedTypes.filter(t => t.kind !== 'enum') as ClassOrStruct[]);

    // Build inheritance structure
    const inheritance = buildInheritanceMap(
        allExtractedTypes.filter(t => t.kind !== 'enum') as ClassOrStruct[],
        types.filter(t => t.kind !== 'enum') as ClassOrStruct[],
        isInterface
    );

    // Build legacy maps for RTTI switching (still needed)
    const supertypes = buildSupertypesMap(allExtractedTypes.filter(t => t.kind !== 'enum') as ClassOrStruct[], types.filter(t => t.kind !== 'enum') as ClassOrStruct[]);
    const subtypes = buildSubtypesMap(supertypes);

    console.log('Built dart inheritance map with', Object.keys(inheritance).length, 'entries');
    for (const [child, info] of Object.entries(inheritance)) {
        if (child.includes('constraint') && (info.extends || info.mixins.length > 0)) {
            const extendsStr = info.extends ? `extends ${info.extends}` : '';
            const mixinsStr = info.mixins.length > 0 ? `with ${info.mixins.join(', ')}` : '';
            console.log(`  ${child} ${extendsStr} ${mixinsStr}`.trim());
        }
    }

    return { cTypes, cEnums, cArrayTypes, inheritance, supertypes, subtypes, isInterface };
}

/** Build supertypes map for inheritance relationships, including template classes */
function buildSupertypesMap(allTypes: (ClassOrStruct)[], nonTemplateTypes: (ClassOrStruct)[]): Record<string, string[]> {
    const supertypes: Record<string, string[]> = {};
    const typeMap = new Map<string, ClassOrStruct>();

    // Build type lookup map from all types (including templates)
    for (const type of allTypes) {
        typeMap.set(type.name, type);
    }

    // Build non-template type lookup for C names
    const nonTemplateMap = new Map<string, ClassOrStruct>();
    for (const type of nonTemplateTypes) {
        nonTemplateMap.set(type.name, type);
    }

    // For each non-template type, find all non-template ancestors
    for (const type of nonTemplateTypes) {
        const classType = type;
        const cName = `spine_${toSnakeCase(classType.name)}`;
        const supertypeList = findNonTemplateSupertypes(classType, typeMap, nonTemplateMap);

        if (supertypeList.length > 0) {
            supertypes[cName] = supertypeList;
        }
    }

    return supertypes;
}

/** Find all non-template supertypes for a given type */
function findNonTemplateSupertypes(type: ClassOrStruct, typeMap: Map<string, ClassOrStruct>, nonTemplateMap: Map<string, ClassOrStruct>): string[] {
    const visited = new Set<string>();
    const supertypes: string[] = [];

    function traverse(currentType: ClassOrStruct) {
        if (visited.has(currentType.name)) return;
        visited.add(currentType.name);

        if (!currentType.superTypes) return;

        for (const superTypeName of currentType.superTypes) {
            // Extract base type name from templated types like "ConstraintGeneric<...>"
            const baseTypeName = superTypeName.split('<')[0];
            const superType = typeMap.get(baseTypeName);
            if (!superType) continue;

            // If this supertype is not a template and has bindings, add it to supertypes
            if (!superType.isTemplate && nonTemplateMap.has(superType.name)) {
                const cName = `spine_${toSnakeCase(superType.name)}`;
                if (!supertypes.includes(cName)) {
                    supertypes.push(cName);
                }
            }

            // Continue traversing up the chain (through templates too)
            traverse(superType);
        }
    }

    traverse(type);
    return supertypes;
}

/** Build subtypes map from supertypes data */
function buildSubtypesMap(supertypes: Record<string, string[]>): Record<string, string[]> {
    const subtypes: Record<string, string[]> = {};

    // For each type and its supertypes, add this type to each supertype's subtypes list
    for (const [childType, supertypeList] of Object.entries(supertypes)) {
        for (const supertype of supertypeList) {
            if (!subtypes[supertype]) {
                subtypes[supertype] = [];
            }
            if (!subtypes[supertype].includes(childType)) {
                subtypes[supertype].push(childType);
            }
        }
    }

    return subtypes;
}

/** Build interface map to identify pure/interface types */
function buildInterfaceMap(allTypes: ClassOrStruct[]): Record<string, boolean> {
    const interfaces: Record<string, boolean> = {};

    for (const type of allTypes) {
        const cName = `spine_${toSnakeCase(type.name)}`;
        interfaces[cName] = isPureInterface(type);
    }

    return interfaces;
}

/** Build inheritance map with extends/implements structure */
function buildInheritanceMap(
    allTypes: ClassOrStruct[],
    nonTemplateTypes: ClassOrStruct[],
    isInterface: Record<string, boolean>
): Record<string, { extends?: string, mixins: string[] }> {
    const inheritance: Record<string, { extends?: string, mixins: string[] }> = {};
    const typeMap = new Map<string, ClassOrStruct>();
    const nonTemplateMap = new Map<string, ClassOrStruct>();

    // Build type lookup maps
    for (const type of allTypes) {
        typeMap.set(type.name, type);
    }
    for (const type of nonTemplateTypes) {
        nonTemplateMap.set(type.name, type);
    }

    // For each non-template type, determine its extends/implements
    for (const type of nonTemplateTypes) {
        const cName = `spine_${toSnakeCase(type.name)}`;
        const directParents = findDirectNonTemplateParents(type, typeMap, nonTemplateMap);

        const concreteParents = directParents.filter(parent => !isInterface[parent]);
        const interfaceParents = directParents.filter(parent => isInterface[parent]);

        inheritance[cName] = {
            extends: concreteParents.length > 0 ? concreteParents[0] : undefined,
            mixins: interfaceParents
        };

        // Fail hard if multiple concrete parents (can't represent in single inheritance languages)
        if (concreteParents.length > 1) {
            console.error(`\nERROR: ${cName} has multiple concrete parents: ${concreteParents.join(', ')}`);
            console.error(`This cannot be represented in single inheritance languages like Dart, Swift, Java, etc.`);
            console.error(`\nTo fix this, convert one of the concrete parent classes to a pure interface:`);
            for (const parent of concreteParents) {
                const parentTypeName = parent.replace('spine_', '').replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase()).replace(/ /g, '');
                console.error(`  - Make ${parentTypeName} a pure interface (all methods = 0)`);
            }
            console.error(`\nThen update the C++ class hierarchy and rebuild.`);
            throw new Error(`Multiple concrete inheritance detected in ${cName}. Cannot generate bindings for single inheritance languages.`);
        }
    }

    return inheritance;
}

/** Find direct non-template parents for a type */
function findDirectNonTemplateParents(
    type: ClassOrStruct,
    typeMap: Map<string, ClassOrStruct>,
    nonTemplateMap: Map<string, ClassOrStruct>
): string[] {
    if (!type.superTypes) return [];

    const directParents: string[] = [];

    for (const superTypeName of type.superTypes) {
        // Extract base type name from templated types
        const baseTypeName = superTypeName.split('<')[0];
        const superType = typeMap.get(baseTypeName);

        if (!superType) continue;

        if (!superType.isTemplate && nonTemplateMap.has(superType.name)) {
            // Direct non-template parent
            const cName = `spine_${toSnakeCase(superType.name)}`;
            directParents.push(cName);
        } else if (superType.isTemplate) {
            // Template parent - recurse to find its non-template parents
            const inheritedParents = findDirectNonTemplateParents(superType, typeMap, nonTemplateMap);
            directParents.push(...inheritedParents);
        }
    }

    return [...new Set(directParents)]; // Remove duplicates
}

/** Check if a class/struct is a pure interface (all methods are pure virtual or only has constructors/destructors) */
function isPureInterface(type: ClassOrStruct): boolean {
    if (!type.members || type.members.length === 0) {
        return true; // No members = pure interface
    }

    const methods = type.members.filter(m => m.kind === 'method') as Method[];
    const fields = type.members.filter(m => m.kind === 'field');

    // If it has fields, it's not a pure interface
    if (fields.length > 0) {
        return false;
    }

    // Filter out RTTI methods (like getRTTI) which are infrastructure methods
    const nonRttiMethods = methods.filter(method =>
        !method.name.toLowerCase().includes('rtti')
    );

    // If no non-RTTI methods, only constructors/destructors/RTTI = pure interface
    if (nonRttiMethods.length === 0) {
        return true;
    }

    // Check if all non-RTTI methods are pure virtual
    return nonRttiMethods.every(method => method.isPure === true);
}

async function main() {
    // Check if we should just export JSON for debugging
    if (process.argv.includes('--export-json')) {
        // Suppress console output during generation
        const originalLog = console.log;
        console.log = () => {};

        const { cTypes, cEnums, cArrayTypes, inheritance, supertypes, subtypes, isInterface } = await generate();

        // Restore console.log and output JSON
        console.log = originalLog;
        console.log(JSON.stringify({ cTypes, cEnums, cArrayTypes, inheritance, supertypes, subtypes, isInterface }, null, 2));
        return;
    }

    // Generate C types and enums
    const { cTypes, cEnums, cArrayTypes, inheritance, supertypes, subtypes, isInterface } = await generate();

    // Write all files to disk
    const cWriter = new CWriter(path.join(__dirname, '../../src/generated'));
    await cWriter.writeAll(cTypes, cEnums, cArrayTypes, supertypes, subtypes);

    // Format the generated code
    console.log('Formatting generated code...');
    try {
        const { execSync } = await import('child_process');
        const formatterPath = path.join(__dirname, '../../../formatters/format-cpp.sh');
        execSync(formatterPath, { stdio: 'inherit' });
        console.log('Code formatting complete!');
    } catch (error) {
        console.warn('Warning: Code formatting failed:', error);
        console.warn('You may need to run the formatter manually');
    }

    console.log('Code generation complete!');
}

if (import.meta.url === `file://${process.argv[1]}`) {
    // Run the main function if this file is executed directly
    main().catch(error => {
        console.error('Error during code generation:', error);
        process.exit(1);
    });
}