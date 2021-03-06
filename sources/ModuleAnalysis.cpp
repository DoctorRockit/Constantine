// This file is distributed under MIT-LICENSE. See COPYING for details.

#include "ModuleAnalysis.hpp"

#include "DeclarationCollector.hpp"
#include "ScopeAnalysis.hpp"
#include "IsCXXThisExpr.hpp"

#include <iterator>
#include <map>
#include <memory>

#include <clang/AST/AST.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <boost/noncopyable.hpp>
#include <boost/bind.hpp>
#include <boost/range.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm/count_if.hpp>


namespace {

// Report function for pseudo constness analysis.
void EmitWarningMessage(clang::DiagnosticsEngine & DE, char const * const M, clang::DeclaratorDecl const * const V) {
    unsigned const Id =
        DE.getCustomDiagID(clang::DiagnosticsEngine::Warning, M);
    clang::DiagnosticBuilder const DB = DE.Report(V->getLocStart(), Id);
    DB << V->getNameAsString();
    DB.setForceEmit();
}

void ReportVariablePseudoConstness(clang::DiagnosticsEngine & DE, clang::DeclaratorDecl const * const V) {
    static char const * const Message =
        "variable '%0' could be declared as const";
    EmitWarningMessage(DE, Message, V);
}

void ReportFunctionPseudoConstness(clang::DiagnosticsEngine & DE, clang::DeclaratorDecl const * const V) {
    static char const * const Message =
        "function '%0' could be declared as const";
    EmitWarningMessage(DE, Message, V);
}

void ReportFunctionPseudoStaticness(clang::DiagnosticsEngine & DE, clang::DeclaratorDecl const * const V) {
    static char const * const Message =
        "function '%0' could be declared as static";
    EmitWarningMessage(DE, Message, V);
}

// Report function for debug functionality.
void EmitNoteMessage(clang::DiagnosticsEngine & DE, char const * const M, clang::DeclaratorDecl const * const V) {
    unsigned const Id = DE.getCustomDiagID(clang::DiagnosticsEngine::Note, M);
    clang::DiagnosticBuilder const DB = DE.Report(V->getLocStart(), Id);
    DB << V->getNameAsString();
    DB.setForceEmit();
}

void ReportVariableDeclaration(clang::DiagnosticsEngine & DE, clang::DeclaratorDecl const * const V) {
    static char const * const Message = "variable '%0' declared here";
    EmitNoteMessage(DE, Message, V);
}

void ReportFunctionDeclaration(clang::DiagnosticsEngine & DE, clang::DeclaratorDecl const * const V) {
    static char const * const Message = "function '%0' declared here";
    EmitNoteMessage(DE, Message, V);
}


// helper method not to be so verbose.
struct IsItFromMainModule {
    bool operator()(clang::Decl const * const D) const {
        clang::SourceManager const & SM = D->getASTContext().getSourceManager();
        return SM.isFromMainFile(D->getLocation());
    }
};

bool IsJustAMethod(clang::CXXMethodDecl const * const F) {
    return
        (F->isUserProvided())
    &&  (! F->isVirtual())
    &&  (! F->isCopyAssignmentOperator())
    &&  (0 == clang::dyn_cast<clang::CXXConstructorDecl const>(F))
    &&  (0 == clang::dyn_cast<clang::CXXConversionDecl const>(F))
    &&  (0 == clang::dyn_cast<clang::CXXDestructorDecl const>(F));
}


// Pseudo constness analysis detects what variable can be declare as const.
// This analysis runs through multiple scopes. We need to store the state of
// the ongoing analysis. Once the variable was changed can't be const.
class PseudoConstnessAnalysisState : public boost::noncopyable {
public:
    PseudoConstnessAnalysisState()
        : boost::noncopyable()
        , Candidates()
        , Changed()
    { }

    void Eval(ScopeAnalysis const & Analysis, clang::DeclaratorDecl const * const V) {
        if (Analysis.WasChanged(V)) {
            boost::for_each(GetReferedVariables(V),
                boost::bind(&PseudoConstnessAnalysisState::RegisterChange, this, _1));
        } else if (Changed.end() == Changed.find(V)) {
            if (! IsConst(*V)) {
                Candidates.insert(V);
            }
        }
    }

    void GenerateReports(clang::DiagnosticsEngine & DE) const {
        boost::for_each(Candidates | boost::adaptors::filtered(IsItFromMainModule()),
            boost::bind(ReportVariablePseudoConstness, boost::ref(DE), _1));
    }

private:
    static bool IsConst(clang::DeclaratorDecl const & D) {
        return (D.getType().getNonReferenceType().isConstQualified());
    }

    void RegisterChange(clang::DeclaratorDecl const * const V) {
        Candidates.erase(V);
        Changed.insert(V);
    }

private:
    Variables Candidates;
    Variables Changed;
};


// Base class for analysis. Implement function declaration visitor, which visit
// functions only once. The traversal algorithm is calling all methods, which is
// not desired. In case of a CXXMethodDecl, it was calling the VisitFunctionDecl
// and the VisitCXXMethodDecl as well. This dispatching is reworked in this class.
class ModuleVisitor
    : public boost::noncopyable
    , public clang::RecursiveASTVisitor<ModuleVisitor> {
public:
    typedef std::auto_ptr<ModuleVisitor> Ptr;
    static ModuleVisitor::Ptr CreateVisitor(Target);

    virtual ~ModuleVisitor()
    { }

public:
    // public visitor method.
    bool VisitFunctionDecl(clang::FunctionDecl const * const F) {
        if (! (F->isThisDeclarationADefinition()))
            return true;

        if (clang::CXXMethodDecl const * const D = clang::dyn_cast<clang::CXXMethodDecl const>(F)) {
            OnCXXMethodDecl(D);
        } else {
            OnFunctionDecl(F);
        }
        return true;
    }

public:
    // interface methods with different visibilities.
    virtual void Dump(clang::DiagnosticsEngine &) const = 0;

protected:
    virtual void OnFunctionDecl(clang::FunctionDecl const *) = 0;
    virtual void OnCXXMethodDecl(clang::CXXMethodDecl const *) = 0;
};


class DebugFunctionDeclarations
    : public ModuleVisitor {
protected:
    void OnFunctionDecl(clang::FunctionDecl const * const F) {
        Functions.insert(F);
    }

    void OnCXXMethodDecl(clang::CXXMethodDecl const * const F) {
        Functions.insert(F);
    }

    void Dump(clang::DiagnosticsEngine & DE) const {
        boost::for_each(Functions,
            boost::bind(ReportFunctionDeclaration, boost::ref(DE), _1));
    }

protected:
    std::set<clang::FunctionDecl const *> Functions;
};


class DebugVariableDeclarations
    : public ModuleVisitor {
private:
    void OnFunctionDecl(clang::FunctionDecl const * const F) {
        boost::copy(GetVariablesFromContext(F),
            std::insert_iterator<Variables>(Result, Result.begin()));
    }

    void OnCXXMethodDecl(clang::CXXMethodDecl const * const F) {
        boost::copy(GetVariablesFromContext(F, (! IsJustAMethod(F))),
            std::insert_iterator<Variables>(Result, Result.begin()));
        boost::copy(GetVariablesFromRecord(F->getParent()->getCanonicalDecl()),
            std::insert_iterator<Variables>(Result, Result.begin()));
    }

    void Dump(clang::DiagnosticsEngine & DE) const {
        boost::for_each(Result,
            boost::bind(ReportVariableDeclaration, boost::ref(DE), _1));
    }

private:
    Variables Result;
};


class DebugVariableUsages
    : public DebugFunctionDeclarations {
private:
    static void ReportVariableUsage(clang::DiagnosticsEngine & DE, clang::FunctionDecl const * const F) {
        ScopeAnalysis const & Analysis = ScopeAnalysis::AnalyseThis(*(F->getBody()));
        Analysis.DebugReferenced(DE);
    }

    void Dump(clang::DiagnosticsEngine & DE) const {
        boost::for_each(Functions,
            boost::bind(ReportVariableUsage, boost::ref(DE), _1));
    }
};


class DebugVariableChanges
    : public DebugFunctionDeclarations {
private:
    static void ReportVariableUsage(clang::DiagnosticsEngine & DE, clang::FunctionDecl const * const F) {
        ScopeAnalysis const & Analysis = ScopeAnalysis::AnalyseThis(*(F->getBody()));
        Analysis.DebugChanged(DE);
    }

    void Dump(clang::DiagnosticsEngine & DE) const {
        boost::for_each(Functions,
            boost::bind(ReportVariableUsage, boost::ref(DE), _1));
    }
};


class AnalyseVariableUsage
    : public ModuleVisitor {
private:
    void OnFunctionDecl(clang::FunctionDecl const * const F) {
        ScopeAnalysis const & Analysis = ScopeAnalysis::AnalyseThis(*(F->getBody()));
        boost::for_each(GetVariablesFromContext(F),
            boost::bind(&PseudoConstnessAnalysisState::Eval, &State, boost::cref(Analysis), _1));
    }

    void OnCXXMethodDecl(clang::CXXMethodDecl const * const F) {
        clang::CXXRecordDecl const * const RecordDecl =
            F->getParent()->getCanonicalDecl();
        Variables const MemberVariables = GetMemberVariablesAndReferences(RecordDecl, F);
        // check variables first,
        ScopeAnalysis const & Analysis = ScopeAnalysis::AnalyseThis(*(F->getBody()));
        boost::for_each(GetVariablesFromContext(F, (! IsJustAMethod(F))),
            boost::bind(&PseudoConstnessAnalysisState::Eval, &State, boost::cref(Analysis), _1));
        boost::for_each(MemberVariables,
            boost::bind(&PseudoConstnessAnalysisState::Eval, &State, boost::cref(Analysis), _1));
        // then check the method itself.
        if ((! F->isVirtual()) &&
            (! F->isStatic()) &&
            F->isUserProvided() &&
            IsJustAMethod(F)
        ) {
            Methods const MemberFunctions = GetMethodsFromRecord(RecordDecl);
            // check the constness first..
            unsigned int const MemberChanges =
                boost::count_if(MemberVariables,
                    boost::bind(&ScopeAnalysis::WasChanged, &Analysis, _1));
            unsigned int const FunctionChanges =
                boost::count_if(
                    MemberFunctions | boost::adaptors::filtered(IsMutatingMethod()),
                    boost::bind(&ScopeAnalysis::WasReferenced, &Analysis, _1));
            // if it looks const, it might be even static..
            if ((0 == MemberChanges) && (0 == FunctionChanges)) {
                unsigned int const MemberAccess =
                    boost::count_if(MemberVariables,
                        boost::bind(&ScopeAnalysis::WasReferenced, &Analysis, _1));
                unsigned int const FunctionAccess =
                    boost::count_if(
                        MemberFunctions | boost::adaptors::filtered(IsMemberMethod()),
                        boost::bind(&ScopeAnalysis::WasReferenced, &Analysis, _1));
                if ((0 == MemberAccess) && (0 == FunctionAccess) &&
                    (! IsCXXThisExpr::Check(F->getBody()))
                ) {
                    StaticCandidates.insert(F);
                } else if (! F->isConst()) {
                    ConstCandidates.insert(F);
                }
            }
        }
    }

    void Dump(clang::DiagnosticsEngine & DE) const {
        State.GenerateReports(DE);
        boost::for_each(ConstCandidates | boost::adaptors::filtered(IsItFromMainModule()),
            boost::bind(ReportFunctionPseudoConstness, boost::ref(DE), _1));
        boost::for_each(StaticCandidates | boost::adaptors::filtered(IsItFromMainModule()),
            boost::bind(ReportFunctionPseudoStaticness, boost::ref(DE), _1));
    }

private:
    struct IsMutatingMethod {
        bool operator()(clang::CXXMethodDecl const * const F) const {
            return (! F->isStatic()) && (! F->isConst());
        }
    };

    struct IsMemberMethod {
        bool operator()(clang::CXXMethodDecl const * const F) const {
            return (! F->isStatic());
        }
    };

private:
    PseudoConstnessAnalysisState State;
    Methods ConstCandidates;
    Methods StaticCandidates;
};


ModuleVisitor::Ptr ModuleVisitor::CreateVisitor(Target const State) {
    switch (State) {
    case FuncionDeclaration :
        return ModuleVisitor::Ptr( new DebugFunctionDeclarations() );
    case VariableDeclaration :
        return ModuleVisitor::Ptr( new DebugVariableDeclarations() );
    case VariableChanges:
        return ModuleVisitor::Ptr( new DebugVariableChanges() );
    case VariableUsages :
        return ModuleVisitor::Ptr( new DebugVariableUsages() );
    case PseudoConstness :
        return ModuleVisitor::Ptr( new AnalyseVariableUsage() );
    }
}

} // namespace anonymous


ModuleAnalysis::ModuleAnalysis(clang::CompilerInstance const & Compiler, Target const T)
    : boost::noncopyable()
    , clang::ASTConsumer()
    , Reporter(Compiler.getDiagnostics())
    , State(T)
{ }

void ModuleAnalysis::HandleTranslationUnit(clang::ASTContext & Ctx) {
    ModuleVisitor::Ptr const V = ModuleVisitor::CreateVisitor(State);
    V->TraverseDecl(Ctx.getTranslationUnitDecl());
    V->Dump(Reporter);
}
