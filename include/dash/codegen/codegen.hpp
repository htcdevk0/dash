#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "dash/ast/ast.hpp"

namespace dash::codegen
{

    struct CompileOptions
    {
        std::string inputPath;
        std::string outputPath;
        bool emitLLVM{false};
        bool emitObjectOnly{false};
        bool emitShared{false};
        std::vector<std::string> linkProfiles;
        std::vector<std::string> extraLinkArgs;
        bool useDashRuntime{false};
        bool smartLinking{false};
    };

    struct ClassLayout
    {
        bool isStatic{false};
        bool isGroup{false};
        llvm::StructType *type{nullptr};
        std::unordered_map<std::string, std::size_t> fieldIndices;
        std::unordered_map<std::string, core::TypeRef> fieldTypes;
        std::unordered_map<std::string, const ast::Expr *> fieldInitializers;
        std::unordered_map<std::string, bool> fieldMutable;
        std::vector<std::string> fieldOrder;
        llvm::GlobalVariable *staticInstance{nullptr};
        std::unordered_map<std::string, llvm::GlobalVariable *> staticFields;
        std::unordered_map<std::string, ast::MemberFunctionDecl *> methods;
    };

    class CodeGenerator
    {
    public:
        CodeGenerator();

        void compile(ast::Program &program, const CompileOptions &options);

    private:
        void initializeModule(const std::string &moduleName);
        void emitDashSignature();
        void declareClass(ast::ClassDecl &decl);
        void declareGlobal(ast::GlobalVarDecl &decl);
        void emitClassMethods(ast::ClassDecl &decl);
        void declareFunction(const ast::ExternDecl &decl);
        void declareFunction(const ast::FunctionDecl &decl);
        void emitFunction(const ast::FunctionDecl &decl);
        void emitBlock(const ast::BlockStmt &block, bool createScope = true);
        void emitStatement(const ast::Stmt &stmt);

        [[nodiscard]] llvm::Value *emitExpr(const ast::Expr &expr);
        [[nodiscard]] llvm::Value *emitAddressOf(const ast::Expr &expr);
        [[nodiscard]] llvm::Value *emitObjectPointer(const ast::Expr &expr);
        [[nodiscard]] llvm::Value *emitRuntimeVariadicTag(const ast::Expr &expr);
        [[nodiscard]] llvm::Value *adaptExternVarArgForPrintf(const ast::Expr &expr, llvm::Value *value, const std::string &format);
        [[nodiscard]] llvm::Value *castValue(llvm::Value *value, const core::TypeRef &from, const core::TypeRef &to);
        [[nodiscard]] llvm::Value *emitArrayLiteralValue(const ast::ArrayLiteralExpr &expr, const core::TypeRef &targetType);
        [[nodiscard]] llvm::Value *emitInterpolatedStringValue(const ast::InterpolatedStringExpr &expr);
        [[nodiscard]] llvm::Value *emitGroupLiteralValue(const ast::ArrayLiteralExpr &expr, const core::TypeRef &targetType);
        [[nodiscard]] llvm::Value *emitDefaultClassValue(const core::TypeRef &targetType);
        [[nodiscard]] llvm::Value *emitConstructorValue(const ast::ConstructorCallExpr &expr);
        [[nodiscard]] llvm::Value *emitValueOrDefault(const core::TypeRef &targetType, const ast::Expr *initializer);
        [[nodiscard]] llvm::Constant *emitConstantDefaultValue(const core::TypeRef &targetType, const ast::Expr *initializer, const std::string &name);
        [[nodiscard]] llvm::Value *emitArrayDataPointer(llvm::Value *arrayAddress, const core::TypeRef &arrayType);
        [[nodiscard]] llvm::Value *emitArraySizeValue(llvm::Value *arrayAddress);
        [[nodiscard]] llvm::Value *emitArrayCapacityValue(llvm::Value *arrayAddress);
        [[nodiscard]] llvm::Value *emitArrayOwnedValue(llvm::Value *arrayAddress);
        void storeArraySizeValue(llvm::Value *arrayAddress, llvm::Value *value);
        void storeArrayCapacityValue(llvm::Value *arrayAddress, llvm::Value *value);
        void storeArrayOwnedValue(llvm::Value *arrayAddress, llvm::Value *value);
        void storeArrayDataValue(llvm::Value *arrayAddress, llvm::Value *value);
        [[nodiscard]] llvm::Value *ensureMutableArrayStorage(llvm::Value *arrayAddress, const core::TypeRef &arrayType, llvm::Value *minCapacity);
        [[nodiscard]] llvm::Function *getOrCreateMalloc();
        [[nodiscard]] llvm::Function *getOrCreateFree();
        [[nodiscard]] llvm::Function *getOrCreateRealloc();
        [[nodiscard]] llvm::Function *getOrCreateMemmove();
        [[nodiscard]] llvm::Function *getOrCreateSnprintf();
        [[nodiscard]] llvm::Function *getOrCreateStrlen();
        [[nodiscard]] llvm::Function *getOrCreateDashInterpUIntLen();
        [[nodiscard]] llvm::Function *getOrCreateDashInterpWriteUInt();
        [[nodiscard]] llvm::Function *getOrCreateDashInterpWriteInt();
        [[nodiscard]] llvm::Function *getOrCreatePow();
        [[nodiscard]] llvm::Type *toLLVMType(const core::TypeRef &type);
        [[nodiscard]] llvm::AllocaInst *createEntryAlloca(llvm::Function *function, llvm::Type *type, const std::string &name);
        [[nodiscard]] llvm::AllocaInst *lookupLocal(const std::string &name, const core::SourceLocation &location) const;
        [[nodiscard]] core::TypeRef lookupLocalType(const std::string &name, const core::SourceLocation &location) const;
        void declareLocal(const std::string &name, llvm::AllocaInst *alloca, const core::TypeRef &type);
        void pushLocalScope();
        void popLocalScope();
        void populateManualDashAbiLinkSymbols(const std::vector<std::string> &extraLinkArgs);

        void emitLLVMToFile(const std::string &path) const;
        void emitObjectFile(const std::string &path, bool positionIndependent = false);
        [[nodiscard]] std::string buildExtraLinkFlags(const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const;
        void linkExecutable(const std::string &objectPath, const std::string &outputPath, const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const;
        void linkSharedLibrary(const std::string &objectPath, const std::string &outputPath, const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const;

        llvm::LLVMContext context_;
        std::unique_ptr<llvm::Module> module_;
        std::unique_ptr<llvm::IRBuilder<>> builder_;
        std::unordered_map<std::string, llvm::Function *> functions_;
        std::unordered_map<std::string, std::string> functionAbis_;
        std::unordered_map<std::string, std::string> functionSymbolNames_;
        std::unordered_map<std::string, std::string> dashAbiFunctionDefinitionSymbols_;
        std::unordered_map<std::string, std::string> dashAbiFunctionUniqueSymbols_;
        std::unordered_map<std::string, std::string> manualDashAbiFunctionUniqueSymbols_;
        std::unordered_map<std::string, std::string> dashAbiGlobalDefinitionSymbols_;
        std::unordered_map<std::string, std::string> dashAbiGlobalUniqueSymbols_;
        std::unordered_map<std::string, std::string> manualDashAbiGlobalUniqueSymbols_;
        std::vector<std::string> manualDashAbiModuleNames_;
        std::unordered_map<std::string, ClassLayout> classes_;
        std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>> enums_;
        std::unordered_map<std::string, std::vector<core::TypeRef>> functionParameterTypes_;
        std::unordered_map<std::string, llvm::GlobalVariable *> globals_;
        std::unordered_map<std::string, core::TypeRef> globalTypes_;
        std::unordered_map<std::string, std::string> globalSymbolNames_;
        std::unordered_set<std::string> externalGlobalSymbols_;
        std::unordered_set<std::string> manualLinkFunctionSymbols_;
        std::unordered_set<std::string> manualLinkGlobalSymbols_;
        std::unordered_set<std::string> usedExternCSymbols_;
        std::string rootInputPath_;
        std::vector<std::unordered_map<std::string, llvm::AllocaInst *>> localScopes_;
        std::vector<std::unordered_map<std::string, core::TypeRef>> localTypeScopes_;
        llvm::Function *currentFunction_{nullptr};
        bool emitShared_{false};
        core::TypeRef currentReturnType_{};
        ClassLayout *currentClass_{nullptr};
        llvm::StructType *arrayType_{nullptr};
        llvm::Function *mallocFunction_{nullptr};
        llvm::Function *freeFunction_{nullptr};
        llvm::Function *reallocFunction_{nullptr};
        llvm::Function *memmoveFunction_{nullptr};
        llvm::Function *snprintfFunction_{nullptr};
        llvm::Function *strlenFunction_{nullptr};
        llvm::Function *dashInterpUIntLenFunction_{nullptr};
        llvm::Function *dashInterpWriteUIntFunction_{nullptr};
        llvm::Function *dashInterpWriteIntFunction_{nullptr};
        llvm::Function *powFunction_{nullptr};
    };

} // namespace dash::codegen