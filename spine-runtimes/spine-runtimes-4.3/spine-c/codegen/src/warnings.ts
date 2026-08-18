import { Type, Member } from './types';

export interface Warning {
    reason: string;
    locations: string[];
}

/**
 * Manages warnings during code generation, grouping them by type
 */
export class WarningsCollector {
    private warnings = new Map<string, Warning>();

    /**
     * Add a warning for a specific type pattern (e.g., "Array<String>")
     */
    addWarning(pattern: string, reason: string, sources: {type: Type, member: Member}[]) {
        if (!this.warnings.has(pattern)) {
            this.warnings.set(pattern, { reason, locations: [] });
        }
        const warning = this.warnings.get(pattern)!;
        for (const source of sources) {
            warning.locations.push(`${source.type.name}::${source.member.name}`);
        }
    }

    /**
     * Add a single warning with location string
     */
    addSingleWarning(pattern: string, reason: string, location: string) {
        if (!this.warnings.has(pattern)) {
            this.warnings.set(pattern, { reason, locations: [] });
        }
        this.warnings.get(pattern)!.locations.push(location);
    }

    /**
     * Check if there are any warnings
     */
    hasWarnings(): boolean {
        return this.warnings.size > 0;
    }

    /**
     * Get all warnings
     */
    getWarnings(): Map<string, Warning> {
        return this.warnings;
    }

    /**
     * Print warnings to console in a formatted way
     */
    printWarnings(title: string = 'Warnings:') {
        if (this.warnings.size > 0) {
            console.log(`\n${title}`);
            for (const [pattern, info] of this.warnings) {
                console.log(`  ${pattern}: ${info.reason}`);
                console.log('    Found in:');
                for (const location of info.locations) {
                    console.log(`      - ${location}`);
                }
            }
            console.log('');
        }
    }
}