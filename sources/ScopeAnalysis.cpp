// Copyright 2012 by Laszlo Nagy [see file MIT-LICENSE]

#include "ScopeAnalysis.hpp"

#include <boost/bind.hpp>
#include <boost/range.hpp>
#include <boost/range/algorithm/for_each.hpp>

#include <clang/AST/RecursiveASTVisitor.h>

namespace {

// Usage extract method implemented in visitor style.
class UsageExtractor
    : public clang::RecursiveASTVisitor<UsageExtractor> {
public:
    typedef ScopeAnalysis::UsageRef UsageRef;
    typedef std::pair<clang::DeclaratorDecl const *, UsageRef> Usage;

    static Usage GetUsage(clang::Expr const & Expr) {
        Usage Result;
        {
            clang::Stmt const * const Stmt = &Expr;

            UsageExtractor Visitor(Result);
            Visitor.TraverseStmt(const_cast<clang::Stmt*>(Stmt));
        }
        Result.second.second = Expr.getSourceRange();
        return Result;
    }

private:
    UsageExtractor(Usage & In)
        : clang::RecursiveASTVisitor<UsageExtractor>()
        , Result(In)
    { }

    void SetType(clang::QualType const & In) {
        static clang::QualType Empty = clang::QualType();

        clang::QualType & Current = Result.second.first;
        if (Empty == Current) {
            Current = In;
        }
    }

    void SetVariable(clang::Decl const * const Decl) {
        clang::DeclaratorDecl const * & Current = Result.first;

        if (clang::DeclaratorDecl const * const VD =
                clang::dyn_cast<clang::VarDecl const>(Decl)) {
            Current = VD;
        } else if (clang::DeclaratorDecl const * const VD =
                clang::dyn_cast<clang::FieldDecl const>(Decl)) {
            Current = VD;
        }
    }

public:
    // public visitor method.
    bool VisitDeclRefExpr(clang::DeclRefExpr const * const Expr) {
        SetVariable(Expr->getDecl());
        SetType(Expr->getType());
        return true;
    }

    bool VisitCastExpr(clang::CastExpr const * const Expr) {
        SetType(Expr->getType());
        return true;
    }

    bool VisitUnaryOperator(clang::UnaryOperator const * const Expr) {
        switch (Expr->getOpcode()) {
        case clang::UO_AddrOf:
        case clang::UO_Deref:
            SetType(Expr->getType());
        }
        return true;
    }

private:
    Usage & Result;
};

// Collect variable usages. One variable could have been used multiple
// times with different constness of the given type.
class UsageRefCollector {
public:
    UsageRefCollector(ScopeAnalysis::UsageRefsMap & Out)
        : Results(Out)
    { }

protected:
    void AddToResults(clang::Expr const * const Stmt) {
        UsageExtractor::Usage const & U = UsageExtractor::GetUsage(*Stmt);
        return AddToResults(U);
    }

    void AddToResults(UsageExtractor::Usage const & U) {
        if (clang::DeclaratorDecl const * const VD = U.first) {
            ScopeAnalysis::UsageRefsMap::iterator It = Results.find(VD);
            if (Results.end() == It) {
                std::pair<ScopeAnalysis::UsageRefsMap::iterator, bool> R =
                    Results.insert(ScopeAnalysis::UsageRefsMap::value_type(VD, ScopeAnalysis::UsageRefs()));
                It = R.first;
            }
            ScopeAnalysis::UsageRefs & Ls = It->second;
            Ls.push_back(U.second);
        }
    }

    void Report(char const * const M, clang::DiagnosticsEngine & DE) const {
        boost::for_each(Results,
            boost::bind(UsageRefCollector::Report, _1, M, boost::ref(DE)));
    }

private:
    static
    void Report( ScopeAnalysis::UsageRefsMap::value_type const & Var
               , char const * const Message
               , clang::DiagnosticsEngine & DE) {
        unsigned const Id = DE.getCustomDiagID(clang::DiagnosticsEngine::Note, Message);
        ScopeAnalysis::UsageRefs const & Ls = Var.second;
        for (ScopeAnalysis::UsageRefs::const_iterator It(Ls.begin()), End(Ls.end()); It != End; ++It) {
            clang::DiagnosticBuilder DB = DE.Report(It->second.getBegin(), Id);
            DB << Var.first->getNameAsString();
            DB << It->first.getAsString();
            DB.setForceEmit();
        }
    }

private:
    ScopeAnalysis::UsageRefsMap & Results;
};

// Collect all variables which were mutated in the given scope.
// (The scope is given by the TraverseStmt method.)
class VariableChangeCollector
    : public UsageRefCollector
    , public clang::RecursiveASTVisitor<VariableChangeCollector> {
public:
    VariableChangeCollector(ScopeAnalysis::UsageRefsMap & Out)
        : UsageRefCollector(Out)
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

    // Variables are mutated if non-const member function called.
    bool VisitMemberExpr(clang::MemberExpr const * const Stmt) {
        clang::Type const * const T = Stmt->getMemberDecl()->getType().getCanonicalType().getTypePtr();
        if (clang::FunctionProtoType const * const F = T->getAs<clang::FunctionProtoType>()) {
            if (! (F->getTypeQuals() & clang::Qualifiers::Const) ) {
                AddToResults(Stmt);
            }
        }
        return true;
    }

    // Arguments potentially mutated when you pass by-pointer or by-reference.
    bool VisitCallExpr(clang::CallExpr const * const Stmt) {
        if (clang::FunctionDecl const * const F = Stmt->getDirectCallee()) {
            // check the function parameters one by one
            int const Args = std::min(Stmt->getNumArgs(), F->getNumParams());
            for (int It = 0; It < Args; ++It) {
                clang::ParmVarDecl const * const P = F->getParamDecl(It);
                if (IsNonConstReferenced(P->getType())) {
                    // same as AddToResults(*(Stmt->getArg(It))), but..
                    UsageExtractor::Usage U =
                        UsageExtractor::GetUsage( *(Stmt->getArg(It)) );
                    // change the usage type to the parameter declaration.
                    U.second.first = (*(P->getType())).getPointeeType();
                    AddToResults(U);
                }
            }
        }
        return true;
    }

    static bool IsNonConstReferenced(clang::QualType const & Decl) {
        return
            ((*Decl).isReferenceType() || (*Decl).isPointerType())
            && (! (*Decl).getPointeeType().isConstQualified());
    }

    void Report(clang::DiagnosticsEngine & DE) const {
        UsageRefCollector::Report("variable '%0' with type '%1' was changed", DE);
    }
};

// Collect all variables which were accessed in the given scope.
// (The scope is given by the TraverseStmt method.)
class VariableAccessCollector
    : public UsageRefCollector
    , public clang::RecursiveASTVisitor<VariableAccessCollector> {
public:
    VariableAccessCollector(ScopeAnalysis::UsageRefsMap & Out)
        : UsageRefCollector(Out)
        , clang::RecursiveASTVisitor<VariableAccessCollector>()
    { }

    bool VisitDeclRefExpr(clang::DeclRefExpr const * const Stmt) {
        AddToResults(Stmt);
        return true;
    }

    void Report(clang::DiagnosticsEngine & DE) const {
        UsageRefCollector::Report("variable '%0' was used", DE);
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
