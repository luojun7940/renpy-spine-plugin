import * as fs from 'node:fs';
import type { Exclusion } from './types';

/**
 * Loads exclusions from a text file.
 *
 * File format:
 * - Lines starting with # are comments
 * - Empty lines are ignored
 * - Type exclusions: "type: TypeName"
 * - Method exclusions: "method: TypeName::methodName [const]"
 * - Field exclusions: "field: TypeName[::fieldName]"
 * - Field getter exclusions: "field-get: TypeName[::fieldName]"
 * - Field setter exclusions: "field-set: TypeName[::fieldName]"
 *
 * When fieldName is omitted, applies to all fields of that type.
 *
 * Examples:
 * ```
 * # Exclude entire types
 * type: SkeletonClipping
 * type: Triangulator
 *
 * # Exclude specific methods
 * method: AnimationState::setListener
 * method: AnimationState::addListener
 *
 * # Exclude only const version of a method
 * method: BoneData::getSetupPose const
 *
 * # Exclude constructors (allows type but prevents creation)
 * method: AtlasRegion::AtlasRegion
 *
 * # Exclude field accessors
 * field: AtlasRegion::names          # Exclude both getter and setter
 * field-get: SecretData::password    # Exclude only getter
 * field-set: Bone::x                 # Exclude only setter (read-only)
 *
 * # Exclude all field accessors for a type
 * field: RenderCommand               # No field accessors at all
 * field-get: DebugData               # No getters (write-only fields)
 * field-set: RenderCommand           # No setters (read-only fields)
 * ```
 */
export function loadExclusions(filePath: string): Exclusion[] {
    const content = fs.readFileSync(filePath, 'utf8');
    const lines = content.split('\n');
    const exclusions: Exclusion[] = [];

    for (const line of lines) {
        const trimmed = line.trim();

        // Skip empty lines and comments
        if (!trimmed || trimmed.startsWith('#')) continue;

        // Parse type exclusion
        const typeMatch = trimmed.match(/^type:\s*(.+)$/);
        if (typeMatch) {
            exclusions.push({
                kind: 'type',
                typeName: typeMatch[1].trim()
            });
            continue;
        }

        // Parse method exclusion with optional const specification
        // Format: method: Type::method or method: Type::method const
        const methodMatch = trimmed.match(/^method:\s*(.+?)::(.+?)(\s+const)?$/);
        if (methodMatch) {
            const methodName = methodMatch[2].trim();
            const isConst = !!methodMatch[3];

            exclusions.push({
                kind: 'method',
                typeName: methodMatch[1].trim(),
                methodName: methodName,
                isConst: isConst || undefined
            });
            continue;
        }

        // Parse field exclusion (all accessors)
        // Format: field: Type::field or field: Type (for all fields)
        const fieldMatch = trimmed.match(/^field:\s*(.+?)(?:::(.+?))?$/);
        if (fieldMatch) {
            const typeName = fieldMatch[1].trim();
            const fieldName = fieldMatch[2]?.trim();

            if (fieldName) {
                // Specific field
                exclusions.push({
                    kind: 'field',
                    typeName,
                    fieldName
                });
            } else {
                // All fields - add both field-get and field-set for the type
                exclusions.push({
                    kind: 'field-get',
                    typeName,
                    fieldName: '*'  // Special marker for all fields
                });
                exclusions.push({
                    kind: 'field-set',
                    typeName,
                    fieldName: '*'
                });
            }
            continue;
        }

        // Parse field getter exclusion
        // Format: field-get: Type::field or field-get: Type (for all fields)
        const fieldGetMatch = trimmed.match(/^field-get:\s*(.+?)(?:::(.+?))?$/);
        if (fieldGetMatch) {
            exclusions.push({
                kind: 'field-get',
                typeName: fieldGetMatch[1].trim(),
                fieldName: fieldGetMatch[2]?.trim() || '*'
            });
            continue;
        }

        // Parse field setter exclusion
        // Format: field-set: Type::field or field-set: Type (for all fields)
        const fieldSetMatch = trimmed.match(/^field-set:\s*(.+?)(?:::(.+?))?$/);
        if (fieldSetMatch) {
            exclusions.push({
                kind: 'field-set',
                typeName: fieldSetMatch[1].trim(),
                fieldName: fieldSetMatch[2]?.trim() || '*'
            });
        }
    }

    return exclusions;
}

export function isTypeExcluded(typeName: string, exclusions: Exclusion[]): boolean {
    return exclusions.some(ex => ex.kind === 'type' && ex.typeName === typeName);
}

export function isMethodExcluded(typeName: string, methodName: string, exclusions: Exclusion[], method?: { isConst?: boolean }): boolean {
    const isConstMethod = method?.isConst || false;

    const result = exclusions.some(ex => {
        if (ex.kind === 'method' &&
            ex.typeName === typeName &&
            ex.methodName === methodName) {
            // If exclusion doesn't specify const, it matches all
            if (ex.isConst === undefined) return true;
            // Otherwise, it must match the const flag
            return ex.isConst === isConstMethod;
        }
        return false;
    });

    return result;
}

export function isFieldExcluded(typeName: string, fieldName: string, exclusions: Exclusion[]): boolean {
    return exclusions.some(ex => {
        if (ex.kind === 'field' && ex.typeName === typeName && ex.fieldName === fieldName) {
            return true;
        }
        return false;
    });
}

export function isFieldGetterExcluded(typeName: string, fieldName: string, exclusions: Exclusion[]): boolean {
    return exclusions.some(ex => {
        if (ex.kind === 'field-get' && ex.typeName === typeName &&
            (ex.fieldName === fieldName || ex.fieldName === '*')) {
            return true;
        }
        // If the entire field is excluded, getter is also excluded
        if (ex.kind === 'field' && ex.typeName === typeName && ex.fieldName === fieldName) {
            return true;
        }
        return false;
    });
}

export function isFieldSetterExcluded(typeName: string, fieldName: string, exclusions: Exclusion[]): boolean {
    return exclusions.some(ex => {
        if (ex.kind === 'field-set' && ex.typeName === typeName &&
            (ex.fieldName === fieldName || ex.fieldName === '*')) {
            return true;
        }
        // If the entire field is excluded, setter is also excluded
        if (ex.kind === 'field' && ex.typeName === typeName && ex.fieldName === fieldName) {
            return true;
        }
        return false;
    });
}