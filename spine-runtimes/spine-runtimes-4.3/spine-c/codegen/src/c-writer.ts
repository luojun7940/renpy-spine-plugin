import * as fs from 'node:fs';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';
import type { CClassOrStruct, CEnum, CMethod, CParameter } from './c-types';
import { toSnakeCase } from './types';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const LICENSE_HEADER = fs.readFileSync(path.join(__dirname, '../../../spine-cpp/src/spine/Skeleton.cpp'), 'utf8').split('\n').slice(0, 28).join('\n');

/** Generates strings for CClassOrStruct and CEnum, and writes them to files. */
export class CWriter {
    constructor(private outputDir: string) {
        // Clean and recreate output directory
        this.cleanOutputDirectory();
    }

    private cleanOutputDirectory(): void {
        // Remove existing generated directory if it exists
        if (fs.existsSync(this.outputDir)) {
            console.log(`Cleaning ${this.outputDir}...`);
            fs.rmSync(this.outputDir, { recursive: true, force: true });
        }

        // Recreate the directory
        fs.mkdirSync(this.outputDir, { recursive: true });
    }

    writeClassHeader(type: CClassOrStruct): string {
        const lines: string[] = [];

        // Add header guard
        const guardName = `SPINE_${type.name.toUpperCase()}_H`;
        lines.push(`#ifndef ${guardName}`);
        lines.push(`#define ${guardName}`);
        lines.push('');

        // Add includes
        lines.push('#include "../base.h"');
        lines.push('#include "types.h"');
        lines.push('#include "arrays.h"');
        lines.push('');

        // Add extern C
        lines.push('#ifdef __cplusplus');
        lines.push('extern "C" {');
        lines.push('#endif');
        lines.push('');

        // Add all method declarations
        for (const constr of type.constructors) {
            lines.push(this.writeMethodDeclaration(constr));
        }

        if (type.constructors.length > 0) {
            lines.push('');
        }

        if (type.destructor) {
            lines.push(this.writeMethodDeclaration(type.destructor));
            lines.push('');
        }

        for (const method of type.methods) {
            lines.push(this.writeMethodDeclaration(method));
        }

        // Close extern C
        lines.push('');
        lines.push('#ifdef __cplusplus');
        lines.push('}');
        lines.push('#endif');
        lines.push('');
        lines.push(`#endif /* ${guardName} */`);
        lines.push('');

        return lines.join('\n');
    }

    writeClassSource(type: CClassOrStruct): string {
        const lines: string[] = [];

        // Add includes
        lines.push(`#include "${type.name.replace("spine_", "")}.h"`);
        lines.push('#include <spine/spine.h>');
        lines.push('');
        lines.push('using namespace spine;');
        lines.push('');

        // Add all method implementations
        for (const constr of type.constructors) {
            lines.push(this.writeMethodImplementation(constr));
            lines.push('');
        }

        if (type.destructor) {
            lines.push(this.writeMethodImplementation(type.destructor));
            lines.push('');
        }

        for (const method of type.methods) {
            lines.push(this.writeMethodImplementation(method));
            lines.push('');
        }

        return `${lines.join('\n').trim()}\n`;
    }

    writeEnumHeader(enumType: CEnum): string {
        const lines: string[] = [];

        // Add header guard
        const guardName = `SPINE_${enumType.name.toUpperCase()}_H`;
        lines.push(`#ifndef ${guardName}`);
        lines.push(`#define ${guardName}`);
        lines.push('');

        // Add extern C
        lines.push('#ifdef __cplusplus');
        lines.push('extern "C" {');
        lines.push('#endif');
        lines.push('');

        // Add enum declaration
        lines.push(`typedef enum ${enumType.name} {`);

        for (let i = 0; i < enumType.values.length; i++) {
            const value = enumType.values[i];
            const comma = i < enumType.values.length - 1 ? ',' : '';

            if (value.value) {
                lines.push(`    ${value.name} = ${value.value}${comma}`);
            } else {
                lines.push(`    ${value.name}${comma}`);
            }
        }

        lines.push(`} ${enumType.name};`);

        // Close extern C
        lines.push('');
        lines.push('#ifdef __cplusplus');
        lines.push('}');
        lines.push('#endif');
        lines.push('');
        lines.push(`#endif /* ${guardName} */`);
        lines.push('');

        return lines.join('\n');
    }

    private writeMethodDeclaration(method: CMethod): string {
        const params = this.formatParameters(method.parameters);
        const returnTypeWithAnnotation = method.returnTypeNullable ? `/*@null*/ ${method.returnType}` : method.returnType;

        // Generate documentation comment if available
        const docComment = this.formatDocumentationComment(method);
        if (docComment) {
            return `${docComment}\nSPINE_C_API ${returnTypeWithAnnotation} ${method.name}(${params});`;
        }

        return `SPINE_C_API ${returnTypeWithAnnotation} ${method.name}(${params});`;
    }

    private writeMethodImplementation(method: CMethod): string {
        const params = this.formatParameters(method.parameters);
        const returnTypeWithAnnotation = method.returnTypeNullable ? `/*@null*/ ${method.returnType}` : method.returnType;
        const signature = `${returnTypeWithAnnotation} ${method.name}(${params})`;

        return `${signature} {
    ${method.body}
}`;
    }

    private formatParameters(parameters: CParameter[]): string {
        if (parameters.length === 0) {
            return 'void';
        }

        return parameters
            .map(p => {
                const typeWithAnnotation = p.isNullable ? `/*@null*/ ${p.cType}` : p.cType;
                return `${typeWithAnnotation} ${p.name}`;
            })
            .join(', ');
    }

    private formatDocumentationComment(method: CMethod): string | null {
        if (!method.documentation) {
            return null;
        }

        const doc = method.documentation;
        const lines: string[] = [];

        lines.push('/**');

        // Add summary
        if (doc.summary) {
            this.wrapCommentText(doc.summary, lines);
        }

        // Add details if present
        if (doc.details) {
            if (doc.summary) {
                lines.push(' *');
            }
            this.wrapCommentText(doc.details, lines);
        }

        // Add parameter documentation
        if (doc.params && Object.keys(doc.params).length > 0) {
            lines.push(' *');
            for (const [paramName, paramDesc] of Object.entries(doc.params)) {
                // Skip 'self' parameter documentation as it's implicit in C API
                if (paramName === 'self' || paramName === 'this') continue;
                lines.push(` * @param ${paramName} ${paramDesc}`);
            }
        }

        // Add return documentation
        if (doc.returns) {
            lines.push(' *');
            lines.push(` * @return ${doc.returns}`);
        }

        // Add deprecation notice
        if (doc.deprecated) {
            lines.push(' *');
            lines.push(` * @deprecated ${doc.deprecated}`);
        }

        // Add since version
        if (doc.since) {
            lines.push(' *');
            lines.push(` * @since ${doc.since}`);
        }

        // Add see also references
        if (doc.see && doc.see.length > 0) {
            lines.push(' *');
            for (const ref of doc.see) {
                lines.push(` * @see ${ref}`);
            }
        }

        lines.push(' */');

        return lines.join('\n');
    }

    private wrapCommentText(text: string, lines: string[]): void {
        const maxLineLength = 80;
        const prefix = ' * ';
        const maxTextLength = maxLineLength - prefix.length;

        // Split text into paragraphs
        const paragraphs = text.split('\n\n');

        for (let i = 0; i < paragraphs.length; i++) {
            if (i > 0) {
                lines.push(' *');
            }

            const paragraph = paragraphs[i].replace(/\n/g, ' ');
            const words = paragraph.split(' ');
            let currentLine = '';

            for (const word of words) {
                if (currentLine.length === 0) {
                    currentLine = word;
                } else if (currentLine.length + word.length + 1 <= maxTextLength) {
                    currentLine += ' ' + word;
                } else {
                    lines.push(prefix + currentLine);
                    currentLine = word;
                }
            }

            if (currentLine.length > 0) {
                lines.push(prefix + currentLine);
            }
        }
    }

    async writeType(typeName: string, headerContent: string, sourceContent?: string): Promise<void> {
        const fileName = toSnakeCase(typeName);

        // Write header file
        const headerPath = path.join(this.outputDir, `${fileName}.h`);
        fs.writeFileSync(headerPath, headerContent);

        // Write source file (as .cpp since it contains C++ code)
        if (sourceContent) {
            const sourcePath = path.join(this.outputDir, `${fileName}.cpp`);
            fs.writeFileSync(sourcePath, sourceContent);
        }
    }

    async writeMainHeader(cClasses: CClassOrStruct[]): Promise<void> {
        const mainHeaderPath = path.join(this.outputDir, '..', '..', 'include', 'spine-c.h');

        // Ensure include directory exists
        const includeDir = path.dirname(mainHeaderPath);
        if (!fs.existsSync(includeDir)) {
            fs.mkdirSync(includeDir, { recursive: true });
        }

        const lines = [
            LICENSE_HEADER,
            '',
            '#ifndef SPINE_C_H',
            '#define SPINE_C_H',
            '',
            '// Base definitions',
            '#include "../src/base.h"',
            '',
            '// All type declarations and enum includes',
            '#include "../src/generated/types.h"',
            '',
            '// Extension types & functions',
            '#include "../src/extensions.h"',
            '',
            '// Cast functions for type conversions',
            '#include "../src/generated/casts.h"',
            '',
            '// Generated class types'
        ];

        // Add class includes (they contain the actual function declarations)
        for (const classType of cClasses) {
            lines.push(`#include "../src/generated/${classType.name.replace("spine_", "")}.h"`);
        }

        lines.push('');
        lines.push('#endif // SPINE_C_H');
        lines.push('');

        fs.writeFileSync(mainHeaderPath, lines.join('\n'));
    }

    async writeArrays(cArrayTypes: CClassOrStruct[]): Promise<void> {
        console.log('\nGenerating arrays.h/arrays.cpp...');

        // Generate header
        const arrayHeaderLines: string[] = [];

        arrayHeaderLines.push(LICENSE_HEADER);
        arrayHeaderLines.push('');
        arrayHeaderLines.push('#ifndef SPINE_C_ARRAYS_H');
        arrayHeaderLines.push('#define SPINE_C_ARRAYS_H');
        arrayHeaderLines.push('');
        arrayHeaderLines.push('#include "../base.h"');
        arrayHeaderLines.push('#include "types.h"');
        arrayHeaderLines.push('');
        arrayHeaderLines.push('#ifdef __cplusplus');
        arrayHeaderLines.push('extern "C" {');
        arrayHeaderLines.push('#endif');
        arrayHeaderLines.push('');

        // Add opaque type declarations
        for (const arrayType of cArrayTypes) {
            arrayHeaderLines.push(`SPINE_OPAQUE_TYPE(${arrayType.name})`);
        }

        arrayHeaderLines.push('');

        // Add all method declarations
        for (const arrayType of cArrayTypes) {
            arrayHeaderLines.push(arrayType.constructors.map(c => this.writeMethodDeclaration(c)).join('\n\n'));
            if (arrayType.destructor) {
                arrayHeaderLines.push(this.writeMethodDeclaration(arrayType.destructor));
            }
            arrayHeaderLines.push(arrayType.methods.map(c => this.writeMethodDeclaration(c)).join('\n\n'));
            arrayHeaderLines.push('');
        }

        // Close extern C
        arrayHeaderLines.push('#ifdef __cplusplus');
        arrayHeaderLines.push('}');
        arrayHeaderLines.push('#endif');
        arrayHeaderLines.push('');
        arrayHeaderLines.push('#endif /* SPINE_C_ARRAYS_H */');
        arrayHeaderLines.push('');


        // Generate source
        const arraySourceLines: string[] = [];

        arraySourceLines.push(LICENSE_HEADER);
        arraySourceLines.push('');
        arraySourceLines.push('#include "arrays.h"');
        arraySourceLines.push('#include <spine/spine.h>');
        arraySourceLines.push('');
        arraySourceLines.push('using namespace spine;');
        arraySourceLines.push('');

        // Add all method implementations
        for (const arrayType of cArrayTypes) {
            arraySourceLines.push(arrayType.constructors.map(c => this.writeMethodImplementation(c)).join('\n\n'));
            if (arrayType.destructor) {
                arraySourceLines.push(this.writeMethodImplementation(arrayType.destructor));
            }
            arraySourceLines.push(arrayType.methods.map(c => this.writeMethodImplementation(c)).join('\n\n'));
            arraySourceLines.push('');
        }


        const headerPath = path.join(this.outputDir, 'arrays.h');
        const sourcePath = path.join(this.outputDir, 'arrays.cpp');

        fs.writeFileSync(headerPath, arrayHeaderLines.join('\n'));
        fs.writeFileSync(sourcePath, arraySourceLines.join('\n'));
    }

    async writeCasts(supertypes: Record<string, string[]>, subtypes: Record<string, string[]>, 
                     cNameToCppName: Record<string, string>): Promise<void> {
        console.log('\nGenerating casts.h/casts.cpp...');

        // Generate header
        const castHeaderLines: string[] = [];
        
        castHeaderLines.push(LICENSE_HEADER);
        castHeaderLines.push('');
        castHeaderLines.push('#ifndef SPINE_C_CASTS_H');
        castHeaderLines.push('#define SPINE_C_CASTS_H');
        castHeaderLines.push('');
        castHeaderLines.push('#include "../base.h"');
        castHeaderLines.push('#include "types.h"');
        castHeaderLines.push('');
        castHeaderLines.push('#ifdef __cplusplus');
        castHeaderLines.push('extern "C" {');
        castHeaderLines.push('#endif');
        castHeaderLines.push('');
        
        // Generate upcast functions (derived to base)
        castHeaderLines.push('// Upcast functions (derived to base) - always safe');
        for (const [derivedType, supertypeList] of Object.entries(supertypes)) {
            for (const baseType of supertypeList) {
                const funcName = `${derivedType}_cast_to_${baseType.replace('spine_', '')}`;
                castHeaderLines.push(`SPINE_C_API ${baseType} ${funcName}(${derivedType} obj);`);
            }
        }
        
        castHeaderLines.push('');
        castHeaderLines.push('// Downcast functions (base to derived) - user must ensure correct type');
        
        // Generate downcast functions (base to derived)
        for (const [baseType, subtypeList] of Object.entries(subtypes)) {
            for (const derivedType of subtypeList) {
                const funcName = `${baseType}_cast_to_${derivedType.replace('spine_', '')}`;
                castHeaderLines.push(`SPINE_C_API ${derivedType} ${funcName}(${baseType} obj);`);
            }
        }
        
        castHeaderLines.push('');
        castHeaderLines.push('#ifdef __cplusplus');
        castHeaderLines.push('}');
        castHeaderLines.push('#endif');
        castHeaderLines.push('');
        castHeaderLines.push('#endif /* SPINE_C_CASTS_H */');
        castHeaderLines.push('');
        
        // Generate source
        const castSourceLines: string[] = [];
        
        castSourceLines.push(LICENSE_HEADER);
        castSourceLines.push('');
        castSourceLines.push('#include "casts.h"');
        castSourceLines.push('#include <spine/spine.h>');
        castSourceLines.push('');
        castSourceLines.push('using namespace spine;');
        castSourceLines.push('');
        
        // Helper function to get C++ class name from C name
        const toCppClassName = (cName: string): string => {
            const cppName = cNameToCppName[cName];
            if (!cppName) {
                throw new Error(`Cannot find C++ class name for C type '${cName}'. Make sure the type is properly generated.`);
            }
            return cppName;
        };
        
        // Generate upcast implementations
        castSourceLines.push('// Upcast function implementations');
        for (const [derivedType, supertypeList] of Object.entries(supertypes)) {
            const derivedCppClass = toCppClassName(derivedType);
            
            for (const baseType of supertypeList) {
                const baseCppClass = toCppClassName(baseType);
                const funcName = `${derivedType}_cast_to_${baseType.replace('spine_', '')}`;
                
                castSourceLines.push(`${baseType} ${funcName}(${derivedType} obj) {`);
                castSourceLines.push(`    ${derivedCppClass}* derived = (${derivedCppClass}*)obj;`);
                castSourceLines.push(`    ${baseCppClass}* base = static_cast<${baseCppClass}*>(derived);`);
                castSourceLines.push(`    return (${baseType})base;`);
                castSourceLines.push(`}`);
                castSourceLines.push('');
            }
        }
        
        // Generate downcast implementations
        castSourceLines.push('// Downcast function implementations');
        for (const [baseType, subtypeList] of Object.entries(subtypes)) {
            const baseCppClass = toCppClassName(baseType);
            
            for (const derivedType of subtypeList) {
                const derivedCppClass = toCppClassName(derivedType);
                const funcName = `${baseType}_cast_to_${derivedType.replace('spine_', '')}`;
                
                castSourceLines.push(`${derivedType} ${funcName}(${baseType} obj) {`);
                castSourceLines.push(`    ${baseCppClass}* base = (${baseCppClass}*)obj;`);
                castSourceLines.push(`    ${derivedCppClass}* derived = static_cast<${derivedCppClass}*>(base);`);
                castSourceLines.push(`    return (${derivedType})derived;`);
                castSourceLines.push(`}`);
                castSourceLines.push('');
            }
        }
        
        const headerPath = path.join(this.outputDir, 'casts.h');
        const sourcePath = path.join(this.outputDir, 'casts.cpp');
        
        fs.writeFileSync(headerPath, castHeaderLines.join('\n'));
        fs.writeFileSync(sourcePath, castSourceLines.join('\n'));
    }

    /** Writes the types.h file, which includes all class forward declarations and enum types. */
    async writeTypesHeader(cClasses: CClassOrStruct[], cEnums: CEnum[]): Promise<void> {
        const headerPath = path.join(this.outputDir, 'types.h');
        const lines: string[] = [
            LICENSE_HEADER,
            '',
            '#ifndef SPINE_C_TYPES_H',
            '#define SPINE_C_TYPES_H',
            '',
            '#ifdef __cplusplus',
            'extern "C" {',
            '#endif',
            '',
            '#include "../base.h"',
            '',
            '// Forward declarations for all non-enum types'
        ];

        // Forward declare all class types
        for (const classType of cClasses) {
            lines.push(`SPINE_OPAQUE_TYPE(${classType.name})`);
        }

        lines.push('');
        lines.push('// Include all enum types (cannot be forward declared)');

        // Include all enum headers
        for (const enumType of cEnums) {
            lines.push(`#include "${enumType.name.replace("spine_", "")}.h"`);
        }

        lines.push('');
        lines.push('#ifdef __cplusplus');
        lines.push('}');
        lines.push('#endif');
        lines.push('');
        lines.push('#endif // SPINE_C_TYPES_H');
        lines.push('');

        fs.writeFileSync(headerPath, lines.join('\n'));
    }

    async writeAll(cClasses: CClassOrStruct[], cEnums: CEnum[], cArrayTypes: CClassOrStruct[], 
                   supertypes: Record<string, string[]>, subtypes: Record<string, string[]>): Promise<void> {
        await this.writeTypesHeader(cClasses, cEnums);
        await this.writeMainHeader(cClasses);
        await this.writeArrays(cArrayTypes);
        
        // Build C name to C++ name mapping
        const cNameToCppName: Record<string, string> = {};
        for (const cClass of cClasses) {
            cNameToCppName[cClass.name] = cClass.cppType.name;
        }
        
        await this.writeCasts(supertypes, subtypes, cNameToCppName);

        // Write all class files
        for (const classType of cClasses) {
            const headerContent = this.writeClassHeader(classType);
            const sourceContent = this.writeClassSource(classType);
            await this.writeType(classType.name.replace("spine_", ""), headerContent, sourceContent);
        }

        // Write all enum headers
        for (const enumType of cEnums) {
            const headerContent = this.writeEnumHeader(enumType);
            await this.writeType(enumType.name.replace("spine_", ""), headerContent);
        }
    }
}