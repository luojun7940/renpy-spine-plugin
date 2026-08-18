import { isFieldExcluded, isFieldGetterExcluded, isMethodExcluded } from './exclusions';
import { type ArraySpecialization, type Exclusion, isPrimitive, type Member, type Type, toSnakeCase } from './types';
import { WarningsCollector } from './warnings';

// Note: This regex won't correctly parse nested arrays like Array<Array<int>>
// It will match "Array<Array<int>" instead of the full type.
// This is actually OK because we handle nested arrays as unsupported anyway.
const ARRAY_REGEX = /Array<([^>]+)>/g;

/**
 * Extracts Array<T> types from a type string and adds them to the arrayTypes map
 */
function extractArrayTypes(
    typeStr: string | undefined,
    arrayTypes: Map<string, {type: Type, member: Member}[]>,
    type: Type,
    member: Member
) {
    if (!typeStr) return;

    // Reset regex lastIndex to ensure it starts from the beginning
    ARRAY_REGEX.lastIndex = 0;
    let match: RegExpExecArray | null;
    // biome-ignore lint/suspicious/noAssignInExpressions: it's fine
    while ((match = ARRAY_REGEX.exec(typeStr)) !== null) {
        const arrayType = match[0];
        const arrayTypeSources = arrayTypes.get(arrayType) || [];
        arrayTypeSources.push({type, member});
        arrayTypes.set(arrayType, arrayTypeSources);
    }
}

/**
 * Scans included spine-cpp types to find Array<T> specializations
 */
export function scanArraySpecializations(includedTypes: Type[], exclusions: Exclusion[]): ArraySpecialization[] {
    const arrayTypes = new Map<string, {type: Type, member: Member}[]>();
    const warnings = new WarningsCollector();

    // Process all included types
    for (const type of includedTypes) {
        if (type.kind === 'enum') continue;

        for (const member of type.members || []) {
            switch (member.kind) {
                case 'method':
                    extractArrayTypes(member.returnType, arrayTypes, type, member);
                    if (member.parameters) {
                        for (const param of member.parameters) {
                            extractArrayTypes(param.type, arrayTypes, type, member);
                        }
                    }
                    break;
                case 'field':
                    extractArrayTypes(member.type, arrayTypes, type, member);
                    break;
                default:
                    break;
            }
        }
    }

    // Convert to specializations
    const specializations: ArraySpecialization[] = [];

    // Get all enum names from included types
    const enumNames = new Set(includedTypes.filter(t => t.kind === 'enum').map(t => t.name));

    for (const [arrayType, sources] of arrayTypes) {
        const elementMatch = arrayType.match(/Array<(.+)>$/);
        if (!elementMatch) continue;

        const elementType = elementMatch[1].trim();

        // Filter out excluded sources
        const filteredSources = sources.filter(source => {
            const typeName = source.type.name;
            const member = source.member;

            // Check if the entire type is excluded
            if (exclusions.some(e => e.kind === 'type' && e.typeName === typeName)) {
                return false;
            }

            // Check based on member kind
            switch (member.kind) {
                case 'method':
                    // Check if method is excluded
                    return !isMethodExcluded(typeName, member.name, exclusions, member);

                case 'field':
                    // Check if field is excluded (all accessors)
                    if (isFieldExcluded(typeName, member.name, exclusions)) {
                        return false;
                    }
                    // Check if field getter is excluded
                    if (isFieldGetterExcluded(typeName, member.name, exclusions)) {
                        return false;
                    }
                    // Field is included if at least setter is not excluded
                    return true;

                default:
                    return true;
            }
        });

        // Skip if all sources are excluded
        if (filteredSources.length === 0) {
            continue;
        }

        // For template types, check if element type is a template parameter
        const firstSource = sources[0];
        const sourceType = firstSource.type;
        if (sourceType.kind !== "enum" && sourceType.isTemplate && sourceType.templateParams?.includes(elementType)) {
            // Warn about template placeholders like T, K
            warnings.addWarning(arrayType, `Template class uses generic array with template parameter '${elementType}'`, filteredSources);
            continue;
        }

        // Check for const element types (not allowed in arrays)
        if (elementType.startsWith('const ') || elementType.includes(' const ')) {
            warnings.addWarning(arrayType, "Arrays should not have const element types", filteredSources);
            continue;
        }

        // Check for multi-level pointers (unsupported, should be caught by checkMultiLevelPointers in index.ts)
        const pointerCount = (elementType.match(/\*/g) || []).length;
        if (pointerCount > 1) {
            warnings.addWarning(arrayType, "Multi-level pointers are not supported", filteredSources);
            continue;
        }

        // Determine type characteristics
        const isPointer = elementType.endsWith('*');
        let cleanElementType = isPointer ? elementType.slice(0, -1).trim() : elementType;

        // Remove "class " or "struct " prefix if present
        cleanElementType = cleanElementType.replace(/^(?:class|struct)\s+/, '');
        const isEnum = enumNames.has(cleanElementType) || cleanElementType === 'PropertyId';
        const isPrimPointer = isPointer && isPrimitive(cleanElementType);
        const isPrim = !isPointer && !isEnum && isPrimitive(cleanElementType);

        // Generate C type names
        let cTypeName: string;
        let cElementType: string;

        if (isPrim) {
            cElementType = cleanElementType;
            // Replace whitespace with underscore for multi-word types like "unsigned short"
            cTypeName = `spine_array_${cleanElementType.replace(/\s+/g, '_')}`;
        } else if (isPrimPointer) {
            // Primitive pointer types: keep the pointer in cElementType
            cElementType = elementType; // e.g., "float*"
            // Generate unique name with _ptr suffix
            cTypeName = `spine_array_${cleanElementType.replace(/\s+/g, '_')}_ptr`;
        } else if (isEnum) {
            // Handle enums
            if (cleanElementType === 'PropertyId') {
                cElementType = 'int64_t'; // PropertyId is typedef long long
                cTypeName = 'spine_array_property_id';
            } else {
                // Convert enum name to snake_case
                const snakeCase = toSnakeCase(cleanElementType);
                cElementType = `spine_${snakeCase}`;
                cTypeName = `spine_array_${snakeCase}`;
            }
        } else if (isPointer) {
            // Handle non-primitive pointer types (e.g., Bone*)
            const snakeCase = toSnakeCase(cleanElementType);
            cElementType = `spine_${snakeCase}`;
            cTypeName = `spine_array_${snakeCase}`;
        } else {
            // Check for problematic types
            if (elementType.startsWith('Array<')) {
                // C doesn't support nested templates, would need manual Array<Array<T>> implementation
                warnings.addWarning(arrayType, "C doesn't support nested templates", filteredSources);
                continue;
            }

            if (elementType === 'String') {
                // String arrays should use const char** instead
                warnings.addWarning(arrayType, "String arrays should use const char** in C API", filteredSources);
                continue;
            }

            // Unknown type - throw!
            throw new Error(`Unsupported array element type: ${elementType} in ${arrayType} at ${firstSource.type.name}::${firstSource.member.name}`);
        }

        specializations.push({
            cppType: arrayType,
            elementType: elementType,
            cTypeName: cTypeName,
            cElementType: cElementType,
            isPointer: isPointer,
            isEnum: isEnum,
            isPrimitive: isPrim,
            sourceMember: firstSource.member // Use first occurrence for debugging
        });
    }

    // Print warnings and exit if there are any unsupported types
    if (warnings.hasWarnings()) {
        warnings.printWarnings('Array Generation Errors:');
        console.error('\nERROR: Found unsupported array types that cannot be wrapped in C.');
        console.error('You must either:');
        console.error('  1. Modify the C++ code to avoid these types');
        console.error('  2. Add method/field exclusions to exclusions.txt');
        console.error('\nExample exclusions:');
        console.error('  method: AttachmentTimeline::getAttachmentNames');
        console.error('  field: AtlasRegion::names');
        console.error('  method: DeformTimeline::getVertices');
        console.error('  method: DrawOrderTimeline::getDrawOrders');
        console.error('  method: Skin::findNamesForSlot');
        process.exit(1);
    }

    // Sort specializations: primitives first, then enums, then pointers
    specializations.sort((a, b) => {
        // Sort by type category first
        const getCategory = (spec: ArraySpecialization) => {
            if (spec.isPrimitive) return 0;
            if (spec.isEnum) return 1;
            if (spec.isPointer) return 2;
            // This should never happen - every specialization must be one of the above
            throw new Error(`Invalid ArraySpecialization state for ${spec.cppType}: ` +
                `isPrimitive=${spec.isPrimitive}, isEnum=${spec.isEnum}, isPointer=${spec.isPointer}`);
        };

        const categoryA = getCategory(a);
        const categoryB = getCategory(b);

        if (categoryA !== categoryB) {
            return categoryA - categoryB;
        }

        // Within same category, sort by name
        return a.cTypeName.localeCompare(b.cTypeName);
    });

    // Log found specializations for debugging
    if (specializations.length > 0) {
        console.log('Found array specializations:');
        for (const spec of specializations) {
            console.log(`  - ${spec.cppType} → ${spec.cTypeName} (element: ${spec.cElementType})`);
        }
        console.log('');
    }

    return specializations;
}