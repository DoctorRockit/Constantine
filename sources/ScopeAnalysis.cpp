// This file is distributed under MIT-LICENSE. See COPYING for details.

#include "ScopeAnalysis.hpp"
#include "UsageCollector.hpp"
#include "IsCXXThisExpr.hpp"

#include <clang/AST/RecursiveASTVisitor.h>

namespace {

// Collect all variables which were mutated in the given scope.
// (The scope is given by the TraverseStmt method.)
class VariableChangeCollector
    : public UsageCollector
    , public clang::RecursiveASTVisitor<VariableChangeCollector> {
public:
    VariableChangeCollector(ScopeAnalysis::UsageRefsMap & Out)
        : UsageCollector(Out)
        , clang::RecursiveASTVisitor<VariableChangeCollector>()
    { }

public:
    // Assignments are mutating variables.
    bool VisitBinaryOperator(clang::BinaryOperator const * const Stmt) {
        if (Stmt->isAssignmentOp()) {
            AddToResults(Stmt->getLHS());
        }
        return true;
    }

    // Inc/Dec-rement operator does mutate variables.
    bool VisitUnaryOperator(clang::UnaryOperator const * const Stmt) {
        if (Stmt->isIncrementDecrementOp()) {
            AddToResults(Stmt->getSubExpr());
        }
        return true;
    }

    // Arguments potentially mutated when you pass by-pointer or by-reference.
    bool VisitCXXConstructExpr(clang::CXXConstructExpr const * const Stmt) {
        clang::CXXConstructorDecl const * const F = Stmt->getConstructor();
        // check the function parameters one by one
        unsigned int const Args =
            std::min(Stmt->getNumArgs(), F->getNumParams());
        for (unsigned int It = 0; It < Args; ++It) {
            clang::ParmVarDecl const * const P = F->getParamDecl(It);
            if (IsNonConstReferenced(P->getType())) {
                AddToResults(Stmt->getArg(It),
                             (*(P->getType())).getPointeeType());
            }
        }
        return true;
    }

    // Arguments potentially mutated when you pass by-pointer or by-reference.
    bool VisitCallExpr(clang::CallExpr const * const Stmt) {
        // some function call is a member call and has 'this' as a first
        // argument, which is not checked here.
        unsigned int const Offset = HasThisAsFirstArgument(Stmt) ? 1 : 0;

        if (clang::FunctionDecl const * const F = Stmt->getDirectCallee()) {
            // check the function parameters one by one
            unsigned int const Args =
                std::min(Stmt->getNumArgs(), F->getNumParams());
            for (unsigned int It = 0; It < Args; ++It) {
                clang::ParmVarDecl const * const P = F->getParamDecl(It);
                if (IsNonConstReferenced(P->getType())) {
                    assert(It + Offset <= Stmt->getNumArgs());
                    AddToResults(Stmt->getArg(It + Offset),
                                 (*(P->getType())).getPointeeType());
                }
            }
        }
        return true;
    }

    // Objects are mutated when non const member call happen.
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr const * const Stmt) {
        if (clang::CXXMethodDecl const * const MD = Stmt->getMethodDecl()) {
            if ((! MD->isConst()) && (! MD->isStatic())) {
                AddToResults(Stmt->getImplicitObjectArgument());
            }
        }
        return true;
    }

    // Objects are mutated when non const operator called.
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr const * const Stmt) {
        // the implimentation relies on that here the first argument
        // is the 'this', while it was not the case with CXXMethodDecl.
        if (clang::FunctionDecl const * const F = Stmt->getDirectCallee()) {
            if (clang::CXXMethodDecl const * const MD = clang::dyn_cast<clang::CXXMethodDecl const>(F)) {
                if ((! MD->isConst()) && (! MD->isStatic())) {
                    if (0 < Stmt->getNumArgs()) {
                        AddToResults(Stmt->getArg(0));
                    }
                }
            }
        }
        return true;
    }

    // Placement new change change the pre allocated memory.
    bool VisitCXXNewExpr(clang::CXXNewExpr const * const Stmt) {
        unsigned int const Args = Stmt->getNumPlacementArgs();
        for (unsigned int It = 0; It < Args; ++It) {
            // FIXME: not all placement argument are mutating.
            AddToResults(Stmt->getPlacementArg(It));
        }
        return true;
    }

private:
    static bool IsNonConstReferenced(clang::QualType const & Decl) {
        return
            ((*Decl).isReferenceType() || (*Decl).isPointerType())
            && (! (*Decl).getPointeeType().isConstQualified());
    }

    static bool HasThisAsFirstArgument(clang::CallExpr const * const Stmt) {
        return
            (clang::dyn_cast<clang::CXXOperatorCallExpr const>(Stmt)) &&
            (Stmt->getDirectCallee()) &&
            (clang::dyn_cast<clang::CXXMethodDecl const>(Stmt->getDirectCallee()));
    }

public:
    void Report(clang::DiagnosticsEngine & DE) const {
        UsageCollector::Report("variable '%0' with type '%1' was changed", DE);
    }
};

// Collect all variables which were accessed in the given scope.
// (The scope is given by the TraverseStmt method.)
class VariableAccessCollector
    : public UsageCollector
    , public clang::RecursiveASTVisitor<VariableAccessCollector> {
public:
    VariableAccessCollector(ScopeAnalysis::UsageRefsMap & Out)
        : UsageCollector(Out)
        , clang::RecursiveASTVisitor<VariableAccessCollector>()
    { }

public:
    bool VisitDeclRefExpr(clang::DeclRefExpr const * const Stmt) {
        AddToResults(Stmt);
        return true;
    }

    bool VisitMemberExpr(clang::MemberExpr * const Stmt) {
        if (IsCXXThisExpr::Check(Stmt)) {
            AddToResults(Stmt);
        }
        return true;
    }

public:
    void Report(clang::DiagnosticsEngine & DE) const {
        UsageCollector::Report("symbol '%0' was used", DE);
    }
};

} // namespace anonymous

ScopeAnalysis ScopeAnalysis::AnalyseThis(clang::Stmt const & Stmt) {
    ScopeAnalysis Result;
    {
        VariableChangeCollector Visitor(Result.Changed);
        Visitor.TraverseStmt(const_cast<clang::Stmt*>(&Stmt));
    }
    {
        VariableAccessCollector Visitor(Result.Used);
        Visitor.TraverseStmt(const_cast<clang::Stmt*>(&Stmt));
    }
    return Result;
}

bool ScopeAnalysis::WasChanged(clang::DeclaratorDecl const * const Decl) const {
    return (Changed.end() != Changed.find(Decl));
}

bool ScopeAnalysis::WasReferenced(clang::DeclaratorDecl const * const Decl) const {
    return (Used.end() != Used.find(Decl));
}

void ScopeAnalysis::DebugChanged(clang::DiagnosticsEngine & DE) const {
    ScopeAnalysis Copy = *this;
    {
        VariableChangeCollector const Visitor(Copy.Changed);
        Visitor.Report(DE);
    }
}

void ScopeAnalysis::DebugReferenced(clang::DiagnosticsEngine & DE) const {
    ScopeAnalysis Copy = *this;
    {
        VariableAccessCollector const Visitor(Copy.Used);
        Visitor.Report(DE);
    }
}
