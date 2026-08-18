#!/usr/bin/env node
import * as fs from 'node:fs';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';
import { extractTypes } from './type-extractor';
import { loadExclusions, isTypeExcluded, isMethodExcluded } from './exclusions';
import type { ClassOrStruct, Constructor, Method, Type } from './types';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/**
 * Checks if a type string represents a pointer to a class instance
 */
function isPointerToClass(typeStr: string, allTypes: Type[]): boolean {
    // Remove const, references, and whitespace
    const cleanType = typeStr.replace(/\bconst\b/g, '').replace(/&/g, '').trim();
    
    // Check if it ends with * (pointer)
    if (!cleanType.endsWith('*')) {
        return false;
    }
    
    // Extract the base type (remove the *)
    const baseType = cleanType.replace(/\*+$/, '').trim();
    
    // Check if the base type is a class/struct in our type list
    return allTypes.some(type => 
        type.kind !== 'enum' && type.name === baseType
    );
}

/**
 * Checks if a type string represents a class instance (for return types)
 */
function isClassType(typeStr: string, allTypes: Type[]): boolean {
    // Remove const, references, and whitespace  
    const cleanType = typeStr.replace(/\bconst\b/g, '').replace(/&/g, '').replace(/\*/g, '').trim();
    
    // Check if the base type is a class/struct in our type list
    return allTypes.some(type => 
        type.kind !== 'enum' && type.name === cleanType
    );
}

/**
 * Analyzes all methods to find those with nullable inputs or outputs
 */
function analyzeNullableMethods(): void {
    console.log('Extracting type information...');
    const allTypes = extractTypes();
    
    console.log('Loading exclusions...');
    const exclusions = loadExclusions(path.join(__dirname, '../exclusions.txt'));
    
    const nullableMethods: Array<{
        filename: string;
        line: number;
        signature: string;
        reasons: string[];
    }> = [];
    
    // Map to group methods by signature
    const methodMap = new Map<string, {
        filename: string;
        line: number;
        signature: string;
        reasons: string[];
    }>();
    
    // Process each type
    for (const type of allTypes) {
        if (type.kind === 'enum') continue;
        
        const classType = type as ClassOrStruct;
        if (!classType.members) continue;
        
        // Skip excluded types
        if (isTypeExcluded(classType.name, exclusions)) {
            continue;
        }
        
        // Get the source file name relative to the nullable file location
        const filename = `../../spine-cpp/include/spine/${classType.name}.h`;
        
        // Process each method and constructor
        for (const member of classType.members) {
            if (member.kind !== 'method' && member.kind !== 'constructor') continue;
            
            const method = member as Method | Constructor;
            
            // Skip inherited methods/constructors - we only want those declared in this class
            if (method.fromSupertype) continue;
            
            // Skip excluded methods
            if (isMethodExcluded(classType.name, method.name, exclusions, member.kind === 'method' ? method as Method : undefined)) {
                continue;
            }
            
            const signature = buildMethodSignature(classType.name, method);
            
            const reasons: string[] = [];
            
            // Check return type - if it returns a pointer to a class (constructors don't have return types)
            if (method.kind === 'method' && (method as Method).returnType) {
                const cleanReturnType = (method as Method).returnType!.replace(/\bconst\b/g, '').trim();
                if (isPointerToClass(cleanReturnType, allTypes)) {
                    reasons.push(`returns nullable pointer: ${(method as Method).returnType}`);
                }
            }
            
            // Check parameters - if any parameter is a pointer to a class
            if (method.parameters) {
                for (const param of method.parameters) {
                    if (isPointerToClass(param.type, allTypes)) {
                        reasons.push(`takes nullable parameter '${param.name}': ${param.type}`);
                    }
                }
            }
            
            // If method has nullability issues, add or update in map
            if (reasons.length > 0) {
                const key = `${filename}:${method.loc.line}`;
                if (methodMap.has(key)) {
                    // Merge reasons if method already exists
                    const existing = methodMap.get(key)!;
                    existing.reasons.push(...reasons);
                } else {
                    methodMap.set(key, {
                        filename,
                        line: method.loc.line,
                        signature,
                        reasons
                    });
                }
            }
        }
    }
    
    // Convert map to array and sort by filename and line
    nullableMethods.push(...Array.from(methodMap.values()));
    nullableMethods.sort((a, b) => {
        if (a.filename !== b.filename) {
            return a.filename.localeCompare(b.filename);
        }
        return a.line - b.line;
    });
    
    // Write results to nullable.md file
    const outputPath = path.join(__dirname, '../nullable.md');
    
    const instructions = `# Spine C++ Nullability Cleanup

## Instructions

**Review and Update Process**
For each unchecked checkbox (now with implementations inlined):
1. **Check if already completed** - First check if the item exists in \`last-nullable.md\` and was already marked as completed [x]. If so, mark it as completed and move to the next item.
2. **Present both implementations** from the checkbox
3. **Ask if we need to change the C++ signature** based on Java nullability patterns (y/n)
4. **Make changes if needed**
   - Change the signature in the header file
   - Update the implementation in the corresponding .cpp file
   - Update any calls in test files (tests/HeadlessTest.cpp) to dereference pointers
   - **Note: Skip cpp-lite files** - ignore build errors from spine-cpp-lite/spine-cpp-lite.cpp
   - Run \`../../spine-cpp/build.sh\` to confirm the changes compile successfully
5. **Confirm changes**
   - Summarize what was changed
   - Ask for confirmation that the changes are correct (y/n)
   - If yes, check the checkbox and move to the next item

## Methods to Review

`;

    console.log('Finding implementations for methods...');
    const enrichedMethods = nullableMethods.map((m, index) => {
        if (index % 20 === 0) {
            console.log(`Processing method ${index + 1}/${nullableMethods.length}...`);
        }
        
        const { cppImpl, javaImpl } = findImplementations(m.filename, m.line, m.signature, allTypes);
        
        const reasonsText = m.reasons.join('; ');
        let methodEntry = `- [ ] [${m.filename}:${m.line}](${m.filename}#L${m.line}) ${m.signature} // ${reasonsText}`;
        
        // Add implementations if found
        if (cppImpl !== 'NOT FOUND' || javaImpl !== 'NOT FOUND') {
            methodEntry += '\n  ```cpp';
            if (cppImpl !== 'NOT FOUND') {
                methodEntry += '\n  ' + cppImpl.replace(/\n/g, '\n  ');
            } else {
                methodEntry += '\n  // NOT FOUND';
            }
            methodEntry += '\n  ```';
            
            methodEntry += '\n  ```java';
            if (javaImpl !== 'NOT FOUND') {
                methodEntry += '\n  ' + javaImpl.replace(/\n/g, '\n  ');
            } else {
                methodEntry += '\n  // NOT FOUND';
            }
            methodEntry += '\n  ```';
        }
        
        return methodEntry;
    });
    
    const methodsList = enrichedMethods.join('\n');
    
    fs.writeFileSync(outputPath, instructions + methodsList + '\n');
    
    console.log(`Found ${nullableMethods.length} methods with nullable inputs/outputs`);
    console.log(`Results written to: ${outputPath}`);
    
    // Print summary statistics
    let returnCount = 0;
    let paramCount = 0;
    for (const method of nullableMethods) {
        if (method.reasons.some(r => r.startsWith('returns'))) returnCount++;
        if (method.reasons.some(r => r.startsWith('takes'))) paramCount++;
    }
    
    console.log('\nSummary:');
    console.log(`  methods with nullable returns: ${returnCount}`);
    console.log(`  methods with nullable parameters: ${paramCount}`);
    console.log(`  methods with both: ${returnCount + paramCount - nullableMethods.length}`);
}

/**
 * Builds a method or constructor signature string
 */
function buildMethodSignature(className: string, method: Method | Constructor): string {
    const params = method.parameters?.map(p => `${p.type} ${p.name}`).join(', ') || '';
    
    if (method.kind === 'constructor') {
        return `${className}::${method.name}(${params})`;
    } else {
        const methodType = method as Method;
        const constStr = methodType.isConst ? ' const' : '';
        return `${methodType.returnType || 'void'} ${className}::${method.name}(${params})${constStr}`;
    }
}

/**
 * Finds concrete subclasses of a given class
 */
function findConcreteSubclasses(className: string, allTypes: Type[]): string[] {
    const subclasses: string[] = [];
    for (const type of allTypes) {
        if (type.kind === 'enum') continue;
        const classType = type as ClassOrStruct;
        if (classType.superTypes?.includes(className) && !classType.isAbstract) {
            subclasses.push(classType.name);
        }
    }
    return subclasses;
}

/**
 * Finds C++ and Java implementations for a method
 */
function findImplementations(filename: string, line: number, signature: string, allTypes: Type[]): {
    cppImpl: string;
    javaImpl: string;
} {
    // Extract header filename (e.g., "AnimationState.h" from "../../spine-cpp/include/spine/AnimationState.h")
    const headerFile = path.basename(filename);
    const className = headerFile.replace('.h', '');
    
    // For Java search, remove "Generic" suffix if present (C++ template pattern)
    const javaClassName = className.endsWith('Generic') ? className.slice(0, -7) : className;
    
    // Extract method name from signature
    const methodMatch = signature.match(/\w+::(\w+)\(/);
    const methodName = methodMatch?.[1] || '';
    
    // Check if this class is abstract
    const classType = allTypes.find(t => t.kind !== 'enum' && (t as ClassOrStruct).name === className) as ClassOrStruct;
    const isAbstract = classType?.isAbstract;
    
    // Map to C++ implementation file
    const cppFile = filename.replace('/include/', '/src/').replace('.h', '.cpp');
    
    let cppImpl = 'NOT FOUND';
    let javaImpl = 'NOT FOUND';
    
    // Find C++ implementation
    const grepPattern = `${className}::${methodName}`;
    try {
        if (fs.existsSync(cppFile)) {
            const cppResult = execSync(`rg -n -A 9 "${grepPattern}" "${cppFile}"`, { encoding: 'utf8' });
            // Take only the first match and limit to 10 lines
            const lines = cppResult.trim().split('\n').slice(0, 10);
            const lineNum = lines[0].split(':')[0];
            cppImpl = `${cppFile}:${lineNum}\n${lines.join('\n')}`;
        } else if (isAbstract) {
            const subclasses = findConcreteSubclasses(className, allTypes);
            if (subclasses.length > 0) {
                const subclassFiles = subclasses.map(sc => `../../spine-cpp/src/spine/${sc}.cpp`).join(', ');
                cppImpl = `PURE VIRTUAL - concrete implementations in: ${subclassFiles}`;
            } else {
                cppImpl = `PURE VIRTUAL - no concrete subclasses found`;
            }
        } else {
            cppImpl = `NOT FOUND - file does not exist: ${cppFile}`;
        }
    } catch (error) {
        if (isAbstract) {
            const subclasses = findConcreteSubclasses(className, allTypes);
            if (subclasses.length > 0) {
                const subclassFiles = subclasses.map(sc => `../../spine-cpp/src/spine/${sc}.cpp`).join(', ');
                cppImpl = `PURE VIRTUAL - concrete implementations in: ${subclassFiles}`;
            } else {
                cppImpl = `PURE VIRTUAL - no concrete subclasses found`;
            }
        } else {
            cppImpl = `NOT FOUND - searched for pattern "${grepPattern}" in ${cppFile}`;
        }
    }
    
    // Find Java implementation - search broadly across all packages
    try {
        // First try the standard location
        const javaFile = `../../spine-libgdx/spine-libgdx/src/com/esotericsoftware/spine/${javaClassName}.java`;
        if (fs.existsSync(javaFile)) {
            const javaPattern = `\\b${methodName}\\s*\\(`;
            const javaResult = execSync(`rg -n -A 9 "${javaPattern}" "${javaFile}"`, { encoding: 'utf8' });
            // Take only the first match and limit to 10 lines
            const lines = javaResult.trim().split('\n').slice(0, 10);
            const lineNum = lines[0].split(':')[0];
            javaImpl = `${javaFile}:${lineNum}\n${lines.join('\n')}`;
        } else {
            throw new Error('File not found in standard location');
        }
    } catch (error) {
        // Search broadly for the class across all packages
        try {
            const findResult = execSync(`rg -l "(class|interface) ${javaClassName}" ../../spine-libgdx/spine-libgdx/src/`, { encoding: 'utf8' });
            const foundJavaFile = findResult.trim().split('\n')[0];
            if (foundJavaFile) {
                const javaPattern = `\\b${methodName}\\s*\\(`;
                const javaResult = execSync(`rg -n -A 9 "${javaPattern}" "${foundJavaFile}"`, { encoding: 'utf8' });
                // Take only the first match and limit to 10 lines
                const lines = javaResult.trim().split('\n').slice(0, 10);
                const lineNum = lines[0].split(':')[0];
                javaImpl = `${foundJavaFile}:${lineNum}\n${lines.join('\n')}`;
            } else {
                // If class not found, search for the method name across all Java files (might be in a similarly named class)
                try {
                    const methodSearchResult = execSync(`rg -l "\\b${methodName}\\s*\\(" ../../spine-libgdx/spine-libgdx/src/ --type java`, { encoding: 'utf8' });
                    const candidateFiles = methodSearchResult.trim().split('\n').filter(f => 
                        f.includes(javaClassName) || f.toLowerCase().includes(javaClassName.toLowerCase()) ||
                        f.includes(className) || f.toLowerCase().includes(className.toLowerCase())
                    );
                    if (candidateFiles.length > 0) {
                        const javaPattern = `\\b${methodName}\\s*\\(`;
                        const javaResult = execSync(`rg -n -A 9 "${javaPattern}" "${candidateFiles[0]}"`, { encoding: 'utf8' });
                        // Take only the first match and limit to 10 lines
                        const lines = javaResult.trim().split('\n').slice(0, 10);
                        const lineNum = lines[0].split(':')[0];
                        javaImpl = `${candidateFiles[0]}:${lineNum}\n${lines.join('\n')}`;
                    } else {
                        javaImpl = `NOT FOUND - searched for class "${javaClassName}" (from C++ "${className}") and method "${methodName}" across all Java files`;
                    }
                } catch (error) {
                    javaImpl = `NOT FOUND - searched for class "${javaClassName}" (from C++ "${className}") and method "${methodName}" across all Java files`;
                }
            }
        } catch (error) {
            javaImpl = `NOT FOUND - searched for class "${javaClassName}" (from C++ "${className}") and method "${methodName}" across all Java files`;
        }
    }
    
    return { cppImpl, javaImpl };
}

// Main execution
if (import.meta.url === `file://${process.argv[1]}`) {
    try {
        analyzeNullableMethods();
    } catch (error) {
        console.error('Error during analysis:', error);
        process.exit(1);
    }
}