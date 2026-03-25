#include "dash/sema/analyzer.hpp"

#include "dash/core/diagnostic.hpp"

namespace dash::sema
{
    namespace
    {
        [[nodiscard]] bool statementAlwaysReturns(const ast::Stmt &stmt)
        {
            if (dynamic_cast<const ast::ReturnStmt *>(&stmt) != nullptr)
                return true;
            if (const auto *block = dynamic_cast<const ast::BlockStmt *>(&stmt))
            {
                if (block->statements.empty())
                    return false;
                return statementAlwaysReturns(*block->statements.back());
            }
            if (const auto *ifStmt = dynamic_cast<const ast::IfStmt *>(&stmt))
            {
                if (ifStmt->elseBranch == nullptr)
                    return false;
                return statementAlwaysReturns(*ifStmt->thenBlock) && statementAlwaysReturns(*ifStmt->elseBranch);
            }
            return false;
        }

        [[nodiscard]] bool hasVariadicStorage(const std::vector<std::unordered_map<std::string, VariableSymbol>> &scopes, const std::string &name)
        {
            const std::string sizeName = name + "_size";
            const std::string dataName = name + "_data";
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            {
                if (it->contains(sizeName) && it->contains(dataName))
                    return true;
            }
            return false;
        }

        [[nodiscard]] core::TypeRef makeUIntType()
        {
            return core::TypeRef{core::BuiltinTypeKind::UInt, ""};
        }

        [[nodiscard]] core::TypeRef makeArrayType(const core::TypeRef &elementType, std::size_t size, bool hasSize)
        {
            core::TypeRef out{};
            out.kind = core::BuiltinTypeKind::Array;
            out.elementKind = elementType.kind;
            out.elementName = elementType.name;
            out.hasArraySize = hasSize;
            out.arraySize = size;
            return out;
        }
        [[nodiscard]] bool canAccessPrivateMember(const ClassSymbol *currentClass, const ClassSymbol &targetClass)
        {
            return currentClass != nullptr && currentClass->name == targetClass.name;
        }

        [[nodiscard]] bool hasAnnotation(const std::vector<ast::Annotation> &annotations, const std::string &name)
        {
            for (const auto &ann : annotations)
                if (ann.name == name)
                    return true;
            return false;
        }

        [[nodiscard]] std::string annotationArgument(const std::vector<ast::Annotation> &annotations, const std::string &name)
        {
            for (const auto &ann : annotations)
                if (ann.name == name)
                    return ann.argument;
            return {};
        }

        void emitFunctionWarnings(const core::SourceLocation &location, const std::string &name, bool deprecated, bool risky, const std::string &customWarning)
        {
            if (deprecated)
                core::emitWarning(location, "call to deprecated function '" + name + "'");
            if (risky)
                core::emitWarning(location, "call to risky function '" + name + "'");
            if (!customWarning.empty())
                core::emitWarning(location, customWarning);
        }

    }

    void Analyzer::analyze(ast::Program &program)
    {
        functions_.clear();
        privateFunctions_.clear();
        classes_.clear();
        enums_.clear();
        scopes_.clear();
        currentReturnType_ = core::TypeRef{};
        currentClass_ = nullptr;

        pushScope();

        collectClasses(program);
        collectEnums(program);
        collectFunctions(program);
        collectGlobalVariables(program);

        for (auto &decl : program.declarations)
        {
            if (auto *klass = dynamic_cast<ast::ClassDecl *>(decl.get()))
                analyzeClassFields(*klass);
        }

        for (auto &decl : program.declarations)
        {
            if (auto *klass = dynamic_cast<ast::ClassDecl *>(decl.get()))
            {
                for (auto &method : klass->methods)
                    analyzeClassMethod(*klass, method);
            }
            else if (auto *function = dynamic_cast<ast::FunctionDecl *>(decl.get()))
            {
                analyzeFunction(*function);
            }
        }

        const auto mainIt = functions_.find("main");
        if (requireEntryPoint_ && !isSharedBuild_)
        {
            if (mainIt == functions_.end())
                core::throwDiagnostic(program.location, "missing entry point: fn main(): int { ... }");
            if (mainIt->second.returnType.kind != core::BuiltinTypeKind::Int)
                core::throwDiagnostic(program.location, "main must return int");
        }

        popScope();
    }

    void Analyzer::collectClasses(ast::Program &program)
    {
        for (auto &decl : program.declarations)
        {
            auto *klass = dynamic_cast<ast::ClassDecl *>(decl.get());
            if (klass == nullptr)
                continue;
            if (classes_.contains(klass->name))
                core::throwDiagnostic(klass->location, "duplicate class declaration: " + klass->name);
            ClassSymbol symbol;
            symbol.name = klass->name;
            symbol.isStatic = klass->isStatic;
            symbol.isGroup = klass->isGroup;
            for (const auto &field : klass->fields)
            {
                if (symbol.fields.contains(field.name))
                    core::throwDiagnostic(field.location, "duplicate field declaration: " + field.name);
                symbol.fields.emplace(field.name, field);
                symbol.fieldOrder.push_back(field);
            }
            for (const auto &method : klass->methods)
            {
                if (symbol.methods.contains(method.name))
                    core::throwDiagnostic(method.location, "duplicate method declaration: " + method.name);
                symbol.methods.emplace(method.name, &method);
            }
            classes_.emplace(klass->name, std::move(symbol));
        }
    }

    void Analyzer::collectEnums(ast::Program &program)
    {
        for (auto &decl : program.declarations)
        {
            auto *enumDecl = dynamic_cast<ast::EnumDecl *>(decl.get());
            if (enumDecl == nullptr)
                continue;
            if (enums_.contains(enumDecl->name))
                core::throwDiagnostic(enumDecl->location, "duplicate enum declaration: " + enumDecl->name);
            EnumSymbol symbol;
            symbol.name = enumDecl->name;
            for (const auto &item : enumDecl->items)
            {
                if (symbol.items.contains(item.name))
                    core::throwDiagnostic(item.location, "duplicate enum item declaration: " + item.name);
                symbol.items.emplace(item.name, item.value);
            }
            enums_.emplace(enumDecl->name, std::move(symbol));
        }
    }

    void Analyzer::collectFunctions(ast::Program &program)
    {
        for (auto &decl : program.declarations)
        {
            if (auto *externDecl = dynamic_cast<ast::ExternDecl *>(decl.get()))
            {
                if (externDecl->abi != "c" && externDecl->abi != "dash")
                    core::throwDiagnostic(externDecl->location, "unsupported extern ABI: " + externDecl->abi);

                if (externDecl->isPrivate)
                {
                    const std::string privateKey = externDecl->location.file + "::" + externDecl->name;
                    if (privateFunctions_.contains(privateKey))
                        core::throwDiagnostic(externDecl->location, "duplicate private function declaration in same file: " + externDecl->name);
                    privateFunctions_.emplace(privateKey, FunctionSymbol{externDecl->name, externDecl->parameters, externDecl->returnType, true, true, externDecl->location.file, false, false, {}});
                    continue;
                }

                if (functions_.contains(externDecl->name))
                    core::throwDiagnostic(externDecl->location, "duplicate function declaration: " + externDecl->name);
                functions_.emplace(externDecl->name, FunctionSymbol{externDecl->name, externDecl->parameters, externDecl->returnType, true, false, externDecl->location.file, false, false, {}});
                continue;
            }
            if (auto *function = dynamic_cast<ast::FunctionDecl *>(decl.get()))
            {
                if (functions_.contains(function->name))
                    core::throwDiagnostic(function->location, "duplicate function declaration: " + function->name);
                if (function->name == "main")
                    validateMainSignature(*function);
                functions_.emplace(function->name, FunctionSymbol{function->name, function->parameters, function->returnType, false, false, function->location.file, hasAnnotation(function->annotations, "Deprecated"), hasAnnotation(function->annotations, "Risky"), annotationArgument(function->annotations, "Warning")});
            }
        }
    }

    void Analyzer::collectGlobalVariables(ast::Program &program)
    {
        for (auto &decl : program.declarations)
        {
            auto *global = dynamic_cast<ast::GlobalVarDecl *>(decl.get());
            if (global == nullptr)
                continue;

            if (global->isExtern)
            {
                if (global->abi != "c" && global->abi != "dash")
                    core::throwDiagnostic(global->location, "unsupported extern ABI: " + global->abi);
                if (global->type.kind == core::BuiltinTypeKind::Unknown)
                    core::throwDiagnostic(global->location, "extern variable declaration requires an explicit type");
                if (global->initializer != nullptr)
                    core::throwDiagnostic(global->location, "extern variable declaration cannot have an initializer");
                declareVariable(VariableSymbol{global->name, global->type, global->isMutable}, global->location);
                continue;
            }

            if (global->initializer)
            {
                if (auto *arrayLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(global->initializer.get()); arrayLiteral != nullptr && global->type.isArray())
                {
                    if (!arrayLiteral->elements.empty())
                    {
                        for (auto &element : arrayLiteral->elements)
                            (void)analyzeExpr(*element);
                        const auto initializerType = analyzeExpr(*global->initializer);
                        if (!core::isImplicitlyConvertible(initializerType, global->type))
                            core::throwDiagnostic(global->location, "cannot initialize global '" + global->name + "' of type " + core::toString(global->type) + " with value of type " + core::toString(initializerType));
                    }
                    if (global->type.hasArraySize && arrayLiteral->elements.size() > global->type.arraySize)
                        core::throwDiagnostic(global->location, "cannot initialize global '" + global->name + "' of type " + core::toString(global->type) + " with " + std::to_string(arrayLiteral->elements.size()) + " element(s)");
                    global->initializer->inferredType = makeArrayType(global->type.arrayElementType(), arrayLiteral->elements.size(), global->type.hasArraySize);
                }
                else if (auto *groupLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(global->initializer.get()); groupLiteral != nullptr && global->type.kind == core::BuiltinTypeKind::Class)
                {
                    const auto &klass = requireClass(global->type.name, global->location);
                    if (!klass.isGroup)
                        core::throwDiagnostic(global->location, "brace initialization for class type requires a group");
                    if (groupLiteral->elements.size() != klass.fields.size())
                        core::throwDiagnostic(global->location, "group '" + global->type.name + "' expects " + std::to_string(klass.fields.size()) + " initializer value(s)");
                    for (std::size_t i = 0; i < klass.fieldOrder.size(); ++i)
                    {
                        const auto &field = klass.fieldOrder[i];
                        const auto actual = analyzeExpr(*groupLiteral->elements[i]);
                        if (!core::isImplicitlyConvertible(actual, field.type))
                            core::throwDiagnostic(groupLiteral->elements[i]->location, "group field '" + field.name + "' expects " + core::toString(field.type) + ", got " + core::toString(actual));
                    }
                    global->initializer->inferredType = global->type;
                }
                else
                {
                    const auto initializerType = analyzeExpr(*global->initializer);
                    if (global->type.kind == core::BuiltinTypeKind::Unknown)
                    {
                        global->type = initializerType;
                    }
                    else if (!core::isImplicitlyConvertible(initializerType, global->type))
                    {
                        core::throwDiagnostic(global->location, "cannot initialize global '" + global->name + "' of type " + core::toString(global->type) + " with value of type " + core::toString(initializerType));
                    }
                }
            }
            else if (global->type.kind == core::BuiltinTypeKind::Unknown && !global->hasExplicitType)
            {
                core::throwDiagnostic(global->location, "global variable declaration without type requires an initializer");
            }

            if (global->type.isArray())
            {
                if (global->isMutable && global->type.hasArraySize)
                    core::throwDiagnostic(global->location, "let arrays cannot declare a fixed maximum size; use type[]");
                if (global->initializer == nullptr)
                    core::throwDiagnostic(global->location, "array globals require an initializer");
            }

            declareVariable(VariableSymbol{global->name, global->type, global->isMutable}, global->location);
        }
    }

    void Analyzer::analyzeFunction(ast::FunctionDecl &function)
    {
        pushScope();
        currentReturnType_ = function.returnType;
        currentClass_ = nullptr;

        for (const auto &parameter : function.parameters)
        {
            if (parameter.isVariadic)
            {
                declareVariable(VariableSymbol{parameter.name, makeUIntType(), true}, parameter.location);
                declareVariable(VariableSymbol{parameter.name + "_size", makeUIntType(), true}, parameter.location);
                declareVariable(VariableSymbol{parameter.name + "_data", makeUIntType(), true}, parameter.location);
                continue;
            }
            declareVariable(VariableSymbol{parameter.name, parameter.type, !parameter.type.isArray() || parameter.isArrayLet}, parameter.location);
        }

        analyzeBlock(*function.body, false);

        popScope();
    }

    void Analyzer::analyzeClassFields(ast::ClassDecl &klass)
    {
        const auto &classSymbol = requireClass(klass.name, klass.location);
        currentClass_ = &classSymbol;
        pushScope();
        declareVariable(VariableSymbol{"self", core::TypeRef{core::BuiltinTypeKind::Class, klass.name}, true}, klass.location);
        for (auto &field : klass.fields)
        {
            if (field.isExtern)
            {
                if (field.abi != "c" && field.abi != "dash")
                    core::throwDiagnostic(field.location, "unsupported extern ABI: " + field.abi);
                if (!klass.isStatic)
                    core::throwDiagnostic(field.location, "extern class fields currently require a static class");
                if (field.initializer != nullptr)
                    core::throwDiagnostic(field.location, "extern class field declaration cannot have an initializer");
                continue;
            }
            if (field.initializer == nullptr)
                continue;
            if (auto *arrayLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(const_cast<ast::Expr *>(field.initializer)); arrayLiteral != nullptr && field.type.isArray())
            {
                if (field.type.hasArraySize && arrayLiteral->elements.size() > field.type.arraySize)
                    core::throwDiagnostic(field.location, "cannot initialize field '" + field.name + "' of type " + core::toString(field.type) + " with " + std::to_string(arrayLiteral->elements.size()) + " element(s)");
                if (!arrayLiteral->elements.empty())
                {
                    for (auto &element : arrayLiteral->elements)
                        (void)analyzeExpr(*element);
                    const auto initializerType = analyzeExpr(*arrayLiteral);
                    if (!core::isImplicitlyConvertible(initializerType, field.type))
                        core::throwDiagnostic(field.location, "cannot initialize field '" + field.name + "' of type " + core::toString(field.type) + " with value of type " + core::toString(initializerType));
                }
                arrayLiteral->inferredType = makeArrayType(field.type.arrayElementType(), arrayLiteral->elements.size(), field.type.hasArraySize);
                continue;
            }
            if (auto *groupLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(const_cast<ast::Expr *>(field.initializer)); groupLiteral != nullptr && field.type.kind == core::BuiltinTypeKind::Class)
            {
                const auto &fieldClass = requireClass(field.type.name, field.location);
                if (!fieldClass.isGroup)
                    core::throwDiagnostic(field.location, "brace initialization for class field requires a group");
                if (groupLiteral->elements.size() != fieldClass.fields.size())
                    core::throwDiagnostic(field.location, "group '" + field.type.name + "' expects " + std::to_string(fieldClass.fields.size()) + " initializer value(s)");
                for (std::size_t i = 0; i < fieldClass.fieldOrder.size(); ++i)
                {
                    const auto &groupField = fieldClass.fieldOrder[i];
                    const auto actual = analyzeExpr(*groupLiteral->elements[i]);
                    if (!core::isImplicitlyConvertible(actual, groupField.type))
                        core::throwDiagnostic(groupLiteral->elements[i]->location, "group field '" + groupField.name + "' expects " + core::toString(groupField.type) + ", got " + core::toString(actual));
                }
                groupLiteral->inferredType = field.type;
                continue;
            }
            const auto initializerType = analyzeExpr(*const_cast<ast::Expr *>(field.initializer));
            if (!core::isImplicitlyConvertible(initializerType, field.type))
                core::throwDiagnostic(field.location, "cannot initialize field '" + field.name + "' of type " + core::toString(field.type) + " with value of type " + core::toString(initializerType));
        }
        popScope();
        currentClass_ = nullptr;
    }

    void Analyzer::analyzeClassMethod(ast::ClassDecl &klass, ast::MemberFunctionDecl &method)
    {
        if (method.isExtern)
        {
            if (method.abi != "c" && method.abi != "dash")
                core::throwDiagnostic(method.location, "unsupported extern ABI: " + method.abi);
            return;
        }

        pushScope();
        currentReturnType_ = method.returnType;
        currentClass_ = &requireClass(klass.name, klass.location);

        declareVariable(VariableSymbol{"self", core::TypeRef{core::BuiltinTypeKind::Class, klass.name}, true}, method.location);
        for (const auto &parameter : method.parameters)
        {
            if (parameter.isVariadic)
            {
                declareVariable(VariableSymbol{parameter.name, makeUIntType(), true}, parameter.location);
                declareVariable(VariableSymbol{parameter.name + "_size", makeUIntType(), true}, parameter.location);
                declareVariable(VariableSymbol{parameter.name + "_data", makeUIntType(), true}, parameter.location);
                continue;
            }
            declareVariable(VariableSymbol{parameter.name, parameter.type, !parameter.type.isArray() || parameter.isArrayLet}, parameter.location);
        }

        analyzeBlock(*method.body, false);

        currentClass_ = nullptr;
        popScope();
    }

    void Analyzer::analyzeBlock(ast::BlockStmt &block, bool createScope)
    {
        if (createScope)
            pushScope();
        for (auto &stmt : block.statements)
            analyzeStatement(*stmt);
        if (createScope)
            popScope();
    }

    void Analyzer::analyzeStatement(ast::Stmt &stmt)
    {
        if (auto *block = dynamic_cast<ast::BlockStmt *>(&stmt))
        {
            analyzeBlock(*block);
            return;
        }
        if (auto *group = dynamic_cast<ast::DeclGroupStmt *>(&stmt))
        {
            for (auto &sub : group->statements)
                analyzeStatement(*sub);
            return;
        }
        if (dynamic_cast<ast::BreakStmt *>(&stmt) != nullptr)
        {
            return;
        }
        if (auto *switchStmt = dynamic_cast<ast::SwitchStmt *>(&stmt))
        {
            if (switchStmt->lowered != nullptr)
                analyzeStatement(*switchStmt->lowered);
            return;
        }
        if (auto *matchStmt = dynamic_cast<ast::MatchStmt *>(&stmt))
        {
            if (matchStmt->lowered != nullptr)
                analyzeStatement(*matchStmt->lowered);
            return;
        }
        if (auto *variable = dynamic_cast<ast::VariableDeclStmt *>(&stmt))
        {
            if (variable->type.isArray() && variable->isMutable && variable->type.hasArraySize)
                core::throwDiagnostic(variable->location, "let arrays cannot declare a fixed maximum size; use type[]");
            if (variable->initializer)
            {
                if (auto *arrayLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(variable->initializer.get()); arrayLiteral != nullptr && variable->type.isArray())
                {
                    if (!arrayLiteral->elements.empty())
                    {
                        for (auto &element : arrayLiteral->elements)
                            (void)analyzeExpr(*element);
                        const auto initializerType = analyzeExpr(*variable->initializer);
                        if (!core::isImplicitlyConvertible(initializerType, variable->type))
                            core::throwDiagnostic(variable->location, "cannot initialize '" + variable->name + "' of type " + core::toString(variable->type) + " with value of type " + core::toString(initializerType));
                    }
                    if (variable->type.hasArraySize && arrayLiteral->elements.size() > variable->type.arraySize)
                        core::throwDiagnostic(variable->location, "cannot initialize '" + variable->name + "' of type " + core::toString(variable->type) + " with " + std::to_string(arrayLiteral->elements.size()) + " element(s)");
                    variable->initializer->inferredType = makeArrayType(variable->type.arrayElementType(), arrayLiteral->elements.size(), variable->type.hasArraySize);
                    declareVariable(VariableSymbol{variable->name, variable->type, variable->isMutable}, variable->location);
                    return;
                }
                if (auto *groupLiteral = dynamic_cast<ast::ArrayLiteralExpr *>(variable->initializer.get()); groupLiteral != nullptr && variable->type.kind == core::BuiltinTypeKind::Class)
                {
                    const auto &klass = requireClass(variable->type.name, variable->location);
                    if (!klass.isGroup)
                        core::throwDiagnostic(variable->location, "brace initialization for class type requires a group");
                    if (groupLiteral->elements.size() != klass.fields.size())
                        core::throwDiagnostic(variable->location, "group '" + variable->type.name + "' expects " + std::to_string(klass.fields.size()) + " initializer value(s)");
                    for (std::size_t i = 0; i < klass.fieldOrder.size(); ++i)
                    {
                        const auto &field = klass.fieldOrder[i];
                        const auto actual = analyzeExpr(*groupLiteral->elements[i]);
                        if (!core::isImplicitlyConvertible(actual, field.type))
                            core::throwDiagnostic(groupLiteral->elements[i]->location, "group field '" + field.name + "' expects " + core::toString(field.type) + ", got " + core::toString(actual));
                    }
                    variable->initializer->inferredType = variable->type;
                    declareVariable(VariableSymbol{variable->name, variable->type, variable->isMutable}, variable->location);
                    return;
                }
                const auto initializerType = analyzeExpr(*variable->initializer);
                if (variable->type.kind == core::BuiltinTypeKind::Unknown)
                {
                    variable->type = initializerType;
                    declareVariable(VariableSymbol{variable->name, variable->type, variable->isMutable}, variable->location);
                    return;
                }
                if (!core::isImplicitlyConvertible(initializerType, variable->type))
                    core::throwDiagnostic(variable->location, "cannot initialize '" + variable->name + "' of type " + core::toString(variable->type) + " with value of type " + core::toString(initializerType));
                declareVariable(VariableSymbol{variable->name, variable->type, variable->isMutable}, variable->location);
                return;
            }
            if (variable->type.kind == core::BuiltinTypeKind::Unknown && !variable->hasExplicitType)
                core::throwDiagnostic(variable->location, "variable declaration without type requires an initializer");
            if (variable->type.isArray())
                core::throwDiagnostic(variable->location, "array variables require an initializer");
            declareVariable(VariableSymbol{variable->name, variable->type, variable->isMutable}, variable->location);
            return;
        }
        if (auto *assignment = dynamic_cast<ast::AssignmentStmt *>(&stmt))
        {
            auto &symbol = requireVariable(assignment->name, assignment->location);
            if (!symbol.isMutable)
                core::throwDiagnostic(assignment->location, "cannot assign to immutable variable '" + assignment->name + "'");
            const auto valueType = analyzeExpr(*assignment->value);
            if (!core::isImplicitlyConvertible(valueType, symbol.type))
                core::throwDiagnostic(assignment->location, "cannot assign value of type " + core::toString(valueType) + " to variable '" + assignment->name + "' of type " + core::toString(symbol.type));
            return;
        }
        if (auto *assignment = dynamic_cast<ast::MemberAssignmentStmt *>(&stmt))
        {
            const auto objectType = analyzeExpr(*assignment->object);
            if (objectType.kind != core::BuiltinTypeKind::Class)
                core::throwDiagnostic(assignment->location, "member assignment requires a class object");
            const auto &klass = requireClass(objectType.name, assignment->location);
            auto it = klass.fields.find(assignment->member);
            if (it == klass.fields.end())
                core::throwDiagnostic(assignment->location, "unknown field: " + assignment->member);
            if (it->second.isPrivate && !canAccessPrivateMember(currentClass_, klass))
                core::throwDiagnostic(assignment->location, "field '" + assignment->member + "' is private in class '" + klass.name + "'");
            if (!it->second.isMutable)
                core::throwDiagnostic(assignment->location, "cannot assign to immutable field '" + assignment->member + "'");
            const auto valueType = analyzeExpr(*assignment->value);
            if (!core::isImplicitlyConvertible(valueType, it->second.type))
                core::throwDiagnostic(assignment->location, "cannot assign value of type " + core::toString(valueType) + " to field '" + assignment->member + "' of type " + core::toString(it->second.type));
            return;
        }
        if (auto *derefAssign = dynamic_cast<ast::DerefAssignmentStmt *>(&stmt))
        {
            const auto pointerType = analyzeExpr(*derefAssign->pointer);
            const auto valueType = analyzeExpr(*derefAssign->value);
            if (pointerType.kind != core::BuiltinTypeKind::Unknown)
            {
                if (!pointerType.isPointer())
                    core::throwDiagnostic(derefAssign->location, "dereference assignment requires a pointer value");
                const auto targetType = pointerType.pointeeType();
                if (!core::isImplicitlyConvertible(valueType, targetType))
                    core::throwDiagnostic(derefAssign->location, "cannot assign value of type " + core::toString(valueType) + " through pointer to " + core::toString(targetType));
            }
            return;
        }
        if (auto *ret = dynamic_cast<ast::ReturnStmt *>(&stmt))
        {
            if (ret->value)
            {
                const auto valueType = analyzeExpr(*ret->value);
                if (valueType.kind != core::BuiltinTypeKind::Unknown && !core::isImplicitlyConvertible(valueType, currentReturnType_))
                    core::throwDiagnostic(ret->location, "cannot return value of type " + core::toString(valueType) + " from function returning " + core::toString(currentReturnType_));
            }
            else if (!currentReturnType_.isVoid())
                core::throwDiagnostic(ret->location, "non-void function must return a value");
            return;
        }
        if (auto *ifStmt = dynamic_cast<ast::IfStmt *>(&stmt))
        {
            const auto conditionType = analyzeExpr(*ifStmt->condition);
            if (!conditionType.isBool() && !conditionType.isNumeric() && conditionType.kind != core::BuiltinTypeKind::Unknown)
                core::throwDiagnostic(ifStmt->condition->location, "if condition must be bool or numeric");
            if (auto *isExpr = dynamic_cast<ast::IsTypeExpr *>(ifStmt->condition.get()))
            {
                const auto &existing = requireVariable(isExpr->variable, isExpr->location);
                pushScope();
                declareVariable(VariableSymbol{isExpr->variable, existing.type.kind == core::BuiltinTypeKind::Unknown ? existing.type : isExpr->type, true}, isExpr->location);
                analyzeBlock(*ifStmt->thenBlock, false);
                popScope();
            }
            else
            {
                analyzeBlock(*ifStmt->thenBlock);
            }
            if (ifStmt->elseBranch != nullptr)
                analyzeStatement(*ifStmt->elseBranch);
            return;
        }
        if (auto *whileStmt = dynamic_cast<ast::WhileStmt *>(&stmt))
        {
            const auto conditionType = analyzeExpr(*whileStmt->condition);
            if (!conditionType.isBool() && !conditionType.isNumeric() && conditionType.kind != core::BuiltinTypeKind::Unknown)
                core::throwDiagnostic(whileStmt->condition->location, "while condition must be bool or numeric");
            analyzeBlock(*whileStmt->body);
            return;
        }
        if (auto *doWhileStmt = dynamic_cast<ast::DoWhileStmt *>(&stmt))
        {
            analyzeBlock(*doWhileStmt->body);
            const auto conditionType = analyzeExpr(*doWhileStmt->condition);
            if (!conditionType.isBool() && !conditionType.isNumeric() && conditionType.kind != core::BuiltinTypeKind::Unknown)
                core::throwDiagnostic(doWhileStmt->condition->location, "do-while condition must be bool or numeric");
            return;
        }
        if (auto *forStmt = dynamic_cast<ast::ForStmt *>(&stmt))
        {
            pushScope();
            if (forStmt->initializer != nullptr)
                analyzeStatement(*forStmt->initializer);
            if (forStmt->condition != nullptr)
            {
                const auto conditionType = analyzeExpr(*forStmt->condition);
                if (!conditionType.isBool() && !conditionType.isNumeric() && conditionType.kind != core::BuiltinTypeKind::Unknown)
                    core::throwDiagnostic(forStmt->condition->location, "for condition must be bool or numeric");
            }
            if (forStmt->increment != nullptr)
                analyzeStatement(*forStmt->increment);
            analyzeBlock(*forStmt->body, false);
            popScope();
            return;
        }
        if (auto *exprStmt = dynamic_cast<ast::ExprStmt *>(&stmt))
        {
            (void)analyzeExpr(*exprStmt->expr);
            return;
        }
        core::throwDiagnostic(stmt.location, "unsupported statement in semantic analysis");
    }

    core::TypeRef Analyzer::analyzeExpr(ast::Expr &expr)
    {
        if (auto *literal = dynamic_cast<ast::IntegerLiteralExpr *>(&expr))
            return expr.inferredType = (literal->forceUnsigned ? core::TypeRef{core::BuiltinTypeKind::UInt, ""} : core::TypeRef{core::BuiltinTypeKind::Int, ""});
        if (dynamic_cast<ast::DoubleLiteralExpr *>(&expr) != nullptr)
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Double, ""};
        if (dynamic_cast<ast::NullLiteralExpr *>(&expr) != nullptr)
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
        if (dynamic_cast<ast::BoolLiteralExpr *>(&expr) != nullptr)
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
        if (dynamic_cast<ast::StringLiteralExpr *>(&expr) != nullptr)
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::String, ""};
        if (auto *interpolated = dynamic_cast<ast::InterpolatedStringExpr *>(&expr))
        {
            for (const auto &embedded : interpolated->expressions)
            {
                const auto type = analyzeExpr(*embedded);
                const bool supported = type.isString() || type.isNumeric() || type.isBool() || type.isChar() || type.isPointer() || type.kind == core::BuiltinTypeKind::Unknown;
                if (!supported)
                    core::throwDiagnostic(embedded->location, "interpolated string expression has unsupported type: " + core::toString(type));
            }
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::String, ""};
        }
        if (dynamic_cast<ast::CharLiteralExpr *>(&expr) != nullptr)
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Char, ""};
        if (auto *array = dynamic_cast<ast::ArrayLiteralExpr *>(&expr))
        {
            if (array->elements.empty())
                core::throwDiagnostic(expr.location, "empty array literals require an explicit array type on the variable");
            auto elementType = analyzeExpr(*array->elements[0]);
            for (std::size_t i = 1; i < array->elements.size(); ++i)
            {
                const auto nextType = analyzeExpr(*array->elements[i]);
                if (elementType.isNumeric() && nextType.isNumeric())
                    elementType = core::usualArithmeticType(elementType, nextType);
                else if (nextType != elementType)
                    core::throwDiagnostic(array->elements[i]->location, "array literal element type mismatch: expected " + core::toString(elementType) + ", got " + core::toString(nextType));
            }
            return expr.inferredType = makeArrayType(elementType, array->elements.size(), true);
        }
        if (auto *variable = dynamic_cast<ast::VariableExpr *>(&expr))
        {
            if (classes_.contains(variable->name))
                return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Class, variable->name};
            return expr.inferredType = requireVariable(variable->name, variable->location).type;
        }
        if (auto *builtin = dynamic_cast<ast::BuiltinDataExpr *>(&expr))
        {
            if (builtin->name == "sizeof")
            {
                if (builtin->arguments.size() != 1)
                    core::throwDiagnostic(builtin->location, "#sizeof(...) expects exactly 1 argument");

                const auto argType = analyzeExpr(*builtin->arguments[0]);

                if (argType.kind == core::BuiltinTypeKind::Unknown &&
                    dynamic_cast<ast::NullLiteralExpr *>(builtin->arguments[0].get()) != nullptr)
                {
                    core::throwDiagnostic(builtin->location, "#sizeof(null) is not valid");
                }

                return expr.inferredType = makeUIntType();
            }

            core::throwDiagnostic(builtin->location, "unknown builtin data expression: #" + builtin->name);
        }
        if (auto *sizeExpr = dynamic_cast<ast::SizeExpr *>(&expr))
        {
            const auto objectType = analyzeExpr(*sizeExpr->object);
            if (objectType.isArray())
                return expr.inferredType = makeUIntType();
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(sizeExpr->object.get()))
            {
                if (hasVariadicStorage(scopes_, var->name))
                    return expr.inferredType = makeUIntType();
            }
            core::throwDiagnostic(sizeExpr->location, "'::size' requires an array or variadic value");
        }
        if (auto *indexExpr = dynamic_cast<ast::IndexExpr *>(&expr))
        {
            const auto objectType = analyzeExpr(*indexExpr->object);
            const auto indexType = analyzeExpr(*indexExpr->index);
            if (!indexType.isNumeric())
                core::throwDiagnostic(indexExpr->index->location, "array/variadic index must be numeric");
            if (objectType.isArray())
                return expr.inferredType = objectType.arrayElementType();
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(indexExpr->object.get()))
            {
                if (hasVariadicStorage(scopes_, var->name))
                    return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
            }
            core::throwDiagnostic(indexExpr->location, "indexing requires an array or variadic value");
        }
        if (auto *push = dynamic_cast<ast::ArrayPushExpr *>(&expr))
        {
            const auto arrayType = analyzeExpr(*push->array);
            if (!arrayType.isArray())
                core::throwDiagnostic(push->location, "::push requires a let array");
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(push->array.get()))
            {
                if (!requireVariable(var->name, var->location).isMutable)
                    core::throwDiagnostic(push->location, "::push requires a mutable let array");
            }
            else
            {
                core::throwDiagnostic(push->location, "::push currently requires an array variable");
            }
            const auto valueType = analyzeExpr(*push->value);
            if (!core::isImplicitlyConvertible(valueType, arrayType.arrayElementType()))
                core::throwDiagnostic(push->value->location, "::push expects " + core::toString(arrayType.arrayElementType()) + ", got " + core::toString(valueType));
            if (push->index)
            {
                const auto indexType = analyzeExpr(*push->index);
                if (!indexType.isNumeric())
                    core::throwDiagnostic(push->index->location, "::push index must be numeric");
            }
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Void, ""};
        }
        if (auto *insert = dynamic_cast<ast::ArrayInsertExpr *>(&expr))
        {
            const auto arrayType = analyzeExpr(*insert->array);
            if (!arrayType.isArray())
                core::throwDiagnostic(insert->location, "::insert requires a let array");
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(insert->array.get()))
            {
                if (!requireVariable(var->name, var->location).isMutable)
                    core::throwDiagnostic(insert->location, "::insert requires a mutable let array");
            }
            else
            {
                core::throwDiagnostic(insert->location, "::insert currently requires an array variable");
            }
            const auto indexType = analyzeExpr(*insert->index);
            if (!indexType.isNumeric())
                core::throwDiagnostic(insert->index->location, "::insert index must be numeric");
            const auto valueType = analyzeExpr(*insert->value);
            if (!core::isImplicitlyConvertible(valueType, arrayType.arrayElementType()))
                core::throwDiagnostic(insert->value->location, "::insert expects " + core::toString(arrayType.arrayElementType()) + ", got " + core::toString(valueType));
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Void, ""};
        }
        if (auto *set = dynamic_cast<ast::ArraySetExpr *>(&expr))
        {
            const auto arrayType = analyzeExpr(*set->array);
            if (!arrayType.isArray())
                core::throwDiagnostic(set->location, "::set requires a let array");
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(set->array.get()))
            {
                if (!requireVariable(var->name, var->location).isMutable)
                    core::throwDiagnostic(set->location, "::set requires a mutable let array");
            }
            else
            {
                core::throwDiagnostic(set->location, "::set currently requires an array variable");
            }
            const auto indexType = analyzeExpr(*set->index);
            if (!indexType.isNumeric())
                core::throwDiagnostic(set->index->location, "::set index must be numeric");
            const auto valueType = analyzeExpr(*set->value);
            if (!core::isImplicitlyConvertible(valueType, arrayType.arrayElementType()))
                core::throwDiagnostic(set->value->location, "::set expects " + core::toString(arrayType.arrayElementType()) + ", got " + core::toString(valueType));
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Void, ""};
        }
        if (auto *rem = dynamic_cast<ast::ArrayRemoveExpr *>(&expr))
        {
            const auto arrayType = analyzeExpr(*rem->array);
            if (!arrayType.isArray())
                core::throwDiagnostic(rem->location, "::rem requires a let array");
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(rem->array.get()))
            {
                if (!requireVariable(var->name, var->location).isMutable)
                    core::throwDiagnostic(rem->location, "::rem requires a mutable let array");
            }
            else
            {
                core::throwDiagnostic(rem->location, "::rem currently requires an array variable");
            }
            const auto indexType = analyzeExpr(*rem->index);
            if (!indexType.isNumeric())
                core::throwDiagnostic(rem->index->location, "::rem index must be numeric");
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Void, ""};
        }
        if (auto *member = dynamic_cast<ast::MemberExpr *>(&expr))
        {
            if (const auto *var = dynamic_cast<ast::VariableExpr *>(member->object.get()))
            {
                if (const auto enumIt = enums_.find(var->name); enumIt != enums_.end())
                {
                    if (!enumIt->second.items.contains(member->member))
                        core::throwDiagnostic(member->location, "unknown enum item: " + member->member);
                    return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Int, ""};
                }
            }
            const auto objectType = analyzeExpr(*member->object);
            if (objectType.kind != core::BuiltinTypeKind::Class)
                core::throwDiagnostic(member->location, "member access requires a class object");
            const auto &klass = requireClass(objectType.name, member->location);
            auto it = klass.fields.find(member->member);
            if (it == klass.fields.end())
                core::throwDiagnostic(member->location, "unknown field: " + member->member);
            if (it->second.isPrivate && !canAccessPrivateMember(currentClass_, klass))
                core::throwDiagnostic(member->location, "field '" + member->member + "' is private in class '" + klass.name + "'");
            return expr.inferredType = it->second.type;
        }
        if (auto *method = dynamic_cast<ast::MethodCallExpr *>(&expr))
        {
            const auto objectType = analyzeExpr(*method->object);
            if (objectType.kind != core::BuiltinTypeKind::Class)
                core::throwDiagnostic(method->location, "method call requires a class object");
            const auto &klass = requireClass(objectType.name, method->location);
            auto it = klass.methods.find(method->method);
            if (it == klass.methods.end())
                core::throwDiagnostic(method->location, "unknown method: " + method->method);
            const auto *decl = it->second;
            if (decl->isPrivate && !canAccessPrivateMember(currentClass_, klass))
                core::throwDiagnostic(method->location, "method '" + method->method + "' is private in class '" + klass.name + "'");
            emitFunctionWarnings(method->location, method->method, hasAnnotation(decl->annotations, "Deprecated"), hasAnnotation(decl->annotations, "Risky"), annotationArgument(decl->annotations, "Warning"));
            const bool variadic = !decl->parameters.empty() && decl->parameters.back().isVariadic;
            const std::size_t fixedCount = variadic ? decl->parameters.size() - 1 : decl->parameters.size();
            std::size_t variadicForwardIndex = static_cast<std::size_t>(-1);
            if ((!variadic && method->arguments.size() != decl->parameters.size()) || (variadic && method->arguments.size() < fixedCount))
                core::throwDiagnostic(method->location, "wrong number of arguments in call to method '" + method->method + "'");
            for (std::size_t i = fixedCount; i < method->arguments.size(); ++i)
            {
                if (auto *forward = dynamic_cast<ast::VariadicForwardExpr *>(method->arguments[i].get()))
                {
                    if (!variadic)
                        core::throwDiagnostic(forward->location, "'(" + forward->name + ")' can only be passed to a variadic method");
                    if (variadicForwardIndex != static_cast<std::size_t>(-1))
                        core::throwDiagnostic(forward->location, "only one variadic forward argument is allowed per call");
                    if (i != fixedCount || method->arguments.size() != fixedCount + 1)
                        core::throwDiagnostic(forward->location, "variadic forward '(...)' must be the only variadic tail argument in the call");
                    if (!hasVariadicStorage(scopes_, forward->name))
                        core::throwDiagnostic(forward->location, "'(" + forward->name + ")' requires a variadic parameter in scope");
                    variadicForwardIndex = i;
                }
            }
            for (std::size_t i = 0; i < fixedCount; ++i)
            {
                auto actual = analyzeExpr(*method->arguments[i]);
                if (!core::isImplicitlyConvertible(actual, decl->parameters[i].type))
                    core::throwDiagnostic(method->arguments[i]->location, "argument " + std::to_string(i + 1) + " of method '" + method->method + "' expects " + core::toString(decl->parameters[i].type) + ", got " + core::toString(actual));
            }
            for (std::size_t i = fixedCount; i < method->arguments.size(); ++i)
            {
                if (i == variadicForwardIndex)
                    continue;
                (void)analyzeExpr(*method->arguments[i]);
            }
            return expr.inferredType = decl->returnType;
        }
        if (auto *unary = dynamic_cast<ast::UnaryExpr *>(&expr))
        {
            const auto operandType = analyzeExpr(*unary->operand);
            if (unary->op == '-')
            {
                if (!operandType.isNumeric())
                    core::throwDiagnostic(unary->location, "unary '-' requires a numeric operand");
                return expr.inferredType = operandType;
            }
            if (unary->op == '&')
            {
                if (dynamic_cast<ast::VariableExpr *>(unary->operand.get()) == nullptr && dynamic_cast<ast::MemberExpr *>(unary->operand.get()) == nullptr)
                    core::throwDiagnostic(unary->location, "address-of '&' currently only supports variables and fields");
                auto out = operandType;
                ++out.pointerDepth;
                return expr.inferredType = out;
            }
            if (unary->op == '!')
            {
                if (!operandType.isBool() && !operandType.isNumeric())
                    core::throwDiagnostic(unary->location, "unary '!' requires an int, uint, double, char, or bool operand");
                return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
            }
            if (unary->op == '*')
            {
                if (operandType.kind == core::BuiltinTypeKind::Unknown)
                    return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
                if (!operandType.isPointer())
                    core::throwDiagnostic(unary->location, "unary '*' requires a pointer operand");
                return expr.inferredType = operandType.pointeeType();
            }
            core::throwDiagnostic(unary->location, "unknown unary operator");
        }
        if (auto *isExpr = dynamic_cast<ast::IsTypeExpr *>(&expr))
        {
            (void)requireVariable(isExpr->variable, isExpr->location);
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
        }
        if (auto *extract = dynamic_cast<ast::ExtractDataExpr *>(&expr))
        {
            const auto operandType = analyzeExpr(*extract->operand);
            if (!operandType.isString() && !operandType.isPointer() && !operandType.isArray() && !operandType.isClass() && operandType.kind != core::BuiltinTypeKind::Unknown)
                core::throwDiagnostic(extract->location, "exdt<...> requires a string, pointer, array, class/group, or raw variadic value");
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::UInt, ""};
        }
        if (auto *parenExpr = dynamic_cast<ast::ParenExpr *>(&expr))
        {
            const auto operandType = analyzeExpr(*parenExpr->operand);
            if (parenExpr->preferVariadicCount)
            {
                if (const auto *var = dynamic_cast<ast::VariableExpr *>(parenExpr->operand.get()))
                {
                    if (hasVariadicStorage(scopes_, var->name))
                        return expr.inferredType = makeUIntType();
                }
            }
            return expr.inferredType = operandType;
        }
        if (auto *forward = dynamic_cast<ast::VariadicForwardExpr *>(&expr))
        {
            if (!hasVariadicStorage(scopes_, forward->name))
                core::throwDiagnostic(forward->location, "'(" + forward->name + ")' requires a variadic parameter in scope");
            return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
        }
        if (auto *cast = dynamic_cast<ast::CastExpr *>(&expr))
        {
            const auto from = analyzeExpr(*cast->operand);
            const auto to = cast->targetType;
            const bool fromPointerLike = from.isPointer() || from.isString();
            const bool toPointerLike = to.isPointer() || to.isString();
            const bool fromPointerIntLike = from.isInt() || from.isUInt() || from.isChar();
            const bool toPointerIntLike = to.isInt() || to.isUInt() || to.isChar();

            if (core::isImplicitlyConvertible(from, to) || core::isImplicitlyConvertible(to, from))
                return expr.inferredType = to;

            if ((fromPointerLike && toPointerLike) ||
                (fromPointerLike && toPointerIntLike) ||
                (toPointerLike && fromPointerIntLike))
                return expr.inferredType = to;

            if (from.kind == core::BuiltinTypeKind::Unknown && (to.isNumeric() || to.isBool() || to.isPointer() || to.isString()))
                return expr.inferredType = to;

            if (from.isClass())
            {
                const auto &sourceClass = requireClass(from.name, cast->location);
                if (sourceClass.fieldOrder.size() == 1)
                {
                    const auto &fieldType = sourceClass.fieldOrder[0].type;
                    if (core::isImplicitlyConvertible(fieldType, to) || core::isImplicitlyConvertible(to, fieldType) || fieldType == to)
                        return expr.inferredType = to;
                }
            }

            if (to.isClass())
            {
                const auto &targetClass = requireClass(to.name, cast->location);
                if (targetClass.fieldOrder.size() == 1)
                {
                    const auto &fieldType = targetClass.fieldOrder[0].type;
                    if (core::isImplicitlyConvertible(from, fieldType) || core::isImplicitlyConvertible(fieldType, from) || fieldType == from)
                        return expr.inferredType = to;
                }
            }

            core::throwDiagnostic(cast->location, "invalid cast from " + core::toString(from) + " to " + core::toString(to));
        }
        if (auto *ternary = dynamic_cast<ast::TernaryExpr *>(&expr))
        {
            const auto condType = analyzeExpr(*ternary->condition);
            if (!condType.isBool() && !condType.isNumeric())
                core::throwDiagnostic(ternary->condition->location, "ternary condition must be bool or numeric");
            const auto thenType = analyzeExpr(*ternary->thenExpr);
            const auto elseType = analyzeExpr(*ternary->elseExpr);
            if (thenType == elseType)
                return expr.inferredType = thenType;
            const auto arithmetic = core::usualArithmeticType(thenType, elseType);
            if (arithmetic.kind != core::BuiltinTypeKind::Unknown)
                return expr.inferredType = arithmetic;
            if (core::isImplicitlyConvertible(thenType, elseType))
                return expr.inferredType = elseType;
            if (core::isImplicitlyConvertible(elseType, thenType))
                return expr.inferredType = thenType;
            core::throwDiagnostic(ternary->location, "ternary branches must have compatible types");
        }
        if (auto *binary = dynamic_cast<ast::BinaryExpr *>(&expr))
        {
            const auto lhs = analyzeExpr(*binary->left);
            const auto rhs = analyzeExpr(*binary->right);
            const bool lhsPointerLike = lhs.isPointer() || lhs.isString();
            const bool rhsPointerLike = rhs.isPointer() || rhs.isString();
            const bool rhsIndexLike = rhs.isInt() || rhs.isUInt();
            const bool lhsIndexLike = lhs.isInt() || lhs.isUInt();
            if (binary->op == "+" || binary->op == "-" || binary->op == "*" || binary->op == "/" || binary->op == "<<" || binary->op == ">>")
            {
                const auto result = core::usualArithmeticType(lhs, rhs);
                if (result.kind != core::BuiltinTypeKind::Unknown)
                    return expr.inferredType = result;

                if (binary->op == "+")
                {
                    if (lhsPointerLike && rhsIndexLike)
                        return expr.inferredType = lhs;
                    if (lhsIndexLike && rhsPointerLike)
                        return expr.inferredType = rhs;
                }

                if (binary->op == "-")
                {
                    if (lhsPointerLike && rhsIndexLike)
                        return expr.inferredType = lhs;
                    if (lhsPointerLike && rhsPointerLike && lhs == rhs)
                        return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Int, ""};
                }

                core::throwDiagnostic(binary->location, "arithmetic operators require numeric operands");
            }
            if (binary->op == "%")
            {
                if (!lhs.isNumeric() || !rhs.isNumeric() || lhs.isDouble() || rhs.isDouble())
                    core::throwDiagnostic(binary->location, "modulo operator requires integer operands");
                return expr.inferredType = core::usualArithmeticType(lhs, rhs);
            }
            if (binary->op == "^")
            {
                const auto result = core::usualArithmeticType(lhs, rhs);
                if (result.kind == core::BuiltinTypeKind::Unknown)
                    core::throwDiagnostic(binary->location, "power operator requires numeric operands");
                return expr.inferredType = result;
            }
            if (binary->op == "<" || binary->op == "<=" || binary->op == ">" || binary->op == ">=")
            {
                if (!lhs.isNumeric() || !rhs.isNumeric())
                    core::throwDiagnostic(binary->location, "comparison operators require numeric operands");
                return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
            }
            if (binary->op == "==" || binary->op == "!=")
            {
                const bool numericComparable = lhs.isNumeric() && rhs.isNumeric();
                const bool sameNonNumeric = lhs == rhs && (lhs.isBool() || lhs.isString() || lhs.kind == core::BuiltinTypeKind::Class || lhs.kind == core::BuiltinTypeKind::Unknown || lhs.isArray() || lhs.isPointer());
                const bool pointerNullComparable = (lhsPointerLike && rhsIndexLike) || (lhsIndexLike && rhsPointerLike);
                if (!numericComparable && !sameNonNumeric && !pointerNullComparable)
                    core::throwDiagnostic(binary->location, "equality operators require matching comparable operands");
                return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
            }
            if (binary->op == "||" || binary->op == "&&")
            {
                const bool lhsTruthy = lhs.isBool() || lhs.isNumeric() || lhs.isPointer() || lhs.isString() || lhs.kind == core::BuiltinTypeKind::Unknown;
                const bool rhsTruthy = rhs.isBool() || rhs.isNumeric() || rhs.isPointer() || rhs.isString() || rhs.kind == core::BuiltinTypeKind::Unknown;
                if (!lhsTruthy || !rhsTruthy)
                    core::throwDiagnostic(binary->location, std::string("'") + binary->op + "' requires bool, numeric, pointer, string, or raw operands");
                return expr.inferredType = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
            }
            core::throwDiagnostic(binary->location, "unsupported binary operator in semantic analysis");
        }
        if (auto *call = dynamic_cast<ast::CallExpr *>(&expr))
        {
            const auto &function = requireFunction(call->callee, call->location);
            emitFunctionWarnings(call->location, call->callee, function.deprecated, function.risky, function.customWarning);
            const bool variadic = !function.parameters.empty() && function.parameters.back().isVariadic;
            const std::size_t fixedCount = variadic ? function.parameters.size() - 1 : function.parameters.size();
            std::size_t variadicForwardIndex = static_cast<std::size_t>(-1);

            if (call->arguments.empty() && fixedCount > 0)
                return expr.inferredType = makeUIntType();

            if ((!variadic && call->arguments.size() != function.parameters.size()) || (variadic && call->arguments.size() < fixedCount))
                core::throwDiagnostic(call->location, "wrong number of arguments in call to '" + call->callee + "'");
            for (std::size_t i = fixedCount; i < call->arguments.size(); ++i)
            {
                if (auto *forward = dynamic_cast<ast::VariadicForwardExpr *>(call->arguments[i].get()))
                {
                    if (!variadic)
                        core::throwDiagnostic(forward->location, "'(" + forward->name + ")' can only be passed to a variadic function");
                    if (variadicForwardIndex != static_cast<std::size_t>(-1))
                        core::throwDiagnostic(forward->location, "only one variadic forward argument is allowed per call");
                    if (i != fixedCount || call->arguments.size() != fixedCount + 1)
                        core::throwDiagnostic(forward->location, "variadic forward '(...)' must be the only variadic tail argument in the call");
                    if (!hasVariadicStorage(scopes_, forward->name))
                        core::throwDiagnostic(forward->location, "'(" + forward->name + ")' requires a variadic parameter in scope");
                    variadicForwardIndex = i;
                }
            }
            for (std::size_t i = 0; i < fixedCount; ++i)
            {
                const auto actual = analyzeExpr(*call->arguments[i]);
                const auto expected = function.parameters[i].type;
                if (!core::isImplicitlyConvertible(actual, expected))
                    core::throwDiagnostic(call->arguments[i]->location, "argument " + std::to_string(i + 1) + " of '" + call->callee + "' expects " + core::toString(expected) + ", got " + core::toString(actual));
            }
            for (std::size_t i = fixedCount; i < call->arguments.size(); ++i)
            {
                if (i == variadicForwardIndex)
                    continue;
                (void)analyzeExpr(*call->arguments[i]);
            }
            return expr.inferredType = function.returnType;
        }
        core::throwDiagnostic(expr.location, "unsupported expression in semantic analysis");
    }

    void Analyzer::validateMainSignature(const ast::FunctionDecl &function) const
    {
        if (function.parameters.size() > 2)
            core::throwDiagnostic(function.location, "main supports at most two parameters right now");
        if (!function.parameters.empty())
        {
            const auto &argc = function.parameters[0].type;
            if (!argc.isInt() && !argc.isUInt())
                core::throwDiagnostic(function.parameters[0].location, "main parameter 1 must be int or uint");
        }
        if (function.parameters.size() == 2)
        {
            const auto &argv = function.parameters[1].type;
            if (!argv.isArray() || argv.elementKind != core::BuiltinTypeKind::String)
                core::throwDiagnostic(function.parameters[1].location, "main parameter 2 must be string[]");
        }
    }

    const FunctionSymbol &Analyzer::requireFunction(const std::string &name, const core::SourceLocation &location) const
    {
        const std::string privateKey = location.file + "::" + name;
        if (const auto privateIt = privateFunctions_.find(privateKey); privateIt != privateFunctions_.end())
            return privateIt->second;

        const auto it = functions_.find(name);
        if (it == functions_.end())
            core::throwDiagnostic(location, "unknown function: " + name);
        return it->second;
    }

    const ClassSymbol &Analyzer::requireClass(const std::string &name, const core::SourceLocation &location) const
    {
        const auto it = classes_.find(name);
        if (it == classes_.end())
            core::throwDiagnostic(location, "unknown class: " + name);
        return it->second;
    }

    VariableSymbol &Analyzer::requireVariable(const std::string &name, const core::SourceLocation &location)
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        {
            const auto found = it->find(name);
            if (found != it->end())
                return found->second;
        }
        core::throwDiagnostic(location, "unknown variable: " + name);
    }

    const VariableSymbol &Analyzer::requireVariable(const std::string &name, const core::SourceLocation &location) const
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
        {
            const auto found = it->find(name);
            if (found != it->end())
                return found->second;
        }
        core::throwDiagnostic(location, "unknown variable: " + name);
    }

    void Analyzer::pushScope() { scopes_.emplace_back(); }
    void Analyzer::popScope() { scopes_.pop_back(); }

    void Analyzer::declareVariable(const VariableSymbol &symbol, const core::SourceLocation &location)
    {
        auto &scope = scopes_.back();
        if (scope.contains(symbol.name))
            core::throwDiagnostic(location, "duplicate variable declaration in same scope: " + symbol.name);
        scope.emplace(symbol.name, symbol);
    }
} // namespace dash::sema
