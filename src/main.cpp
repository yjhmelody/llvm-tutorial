#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/IRBuilder.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token
{
    tok_eof = -1,

    // commands
    tok_def = -2,
    tok_extern = -3,

    // primary
    tok_identifier = -4,
    tok_number = -5
};

// !! global variable for lexer, will be cange by gettok().
static std::string IdentifierStr; // Filled in if tok_identifier
static double NumVal;             // Filled in if tok_number

/// gettok - Return the next token from standard input.
static int gettok()
{
    static int LastChar = ' ';

    // Skip any whitespace.
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar))
    { // identifier: [a-zA-Z][a-zA-Z0-9]*
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;

        if (IdentifierStr == "def")
            return tok_def;
        if (IdentifierStr == "extern")
            return tok_extern;
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.')
    { // Number: [0-9.]+
        std::string NumStr;
        do
        {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    if (LastChar == '#')
    {
        // Comment until end of line.
        do
            LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return gettok();
    }

    // Check for end of file.  Don't eat the EOF.
    if (LastChar == EOF)
        return tok_eof;

    // Otherwise, just return the character as its ascii value.
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//

namespace
{

/// ExprAST - Base class for all expression nodes.
class ExprAST
{
  public:
    virtual ~ExprAST() {}
    // llvm::Value
    virtual Value *codegen() = 0;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
// This allows later phases of the compiler to know what the stored numeric value is.
class NumberExprAST : public ExprAST
{
    double Val;

  public:
    NumberExprAST(double Val) : Val(Val) {}
    virtual Value *codegen();
};
// In the LLVM IR, numeric constants are represented with the ConstantFP class,
// which holds the numeric value in an APFloat internally (APFloat has the capability
// of holding floating point constants of Arbitrary Precision). This code basically just
// creates and returns a ConstantFP. Note that in the LLVM IR that constants are all uniqued together and shared.
// For this reason, the API uses the “foo::get(…)” idiom instead of “new foo(..)” or “foo::Create(..)”.
Value *NumberExprAST::codegen()
{
    return ConstantFP::get(TheContext, APFloat(Val));
}

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST
{
    std::string Name;

  public:
    VariableExprAST(const std::string &Name) : Name(Name) {}
    virtual Value *codegen();
};

// References to variables are also quite simple using LLVM. In the simple version of Kaleidoscope,
// we assume that the variable has already been emitted somewhere and its value is available.
// In practice, the only values that can be in the NamedValues map are function arguments.
// This code simply checks to see that the specified name is in the map (if not, an unknown
// variable is being referenced) and returns the value for it. In future chapters,
// we’ll add support for `loop induction variables` in the symbol table, and for `local variables`.
Value *VariableExprAST::codegen()
{
    Value *V = NamedValued[Name];
    if (V == nullptr)
    {
        LogErrorV("unknow variable name");
    }
    return V;
}

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST
{
    char Op;
    std::unique_ptr<ExprAST> LHS, RHS;

  public:
    BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                  std::unique_ptr<ExprAST> RHS)
        : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
    virtual Value *codegen();
};

// Binary operators start to get more interesting. The basic idea here is that
// we recursively emit code for the left-hand side of the expression, then the
// right-hand side, then we compute the result of the binary expression. In this code,
// we do a simple switch on the opcode to create the right LLVM instruction.

// One nice thing about LLVM is that the name is just a hint. For instance, if the code
// above emits multiple “addtmp” variables, LLVM will automatically provide each one with
// an increasing, unique numeric suffix. Local value names for instructions are purely optional,
// but it makes it much easier to read the IR dumps.

// LLVM instructions are constrained by strict rules: for example, the Left and Right operators of
// an add instruction must have the same type, and the result type of the add must match the operand types.
// Because all values in Kaleidoscope are doubles, this makes for very simple code for add, sub and mul.

// On the other hand, LLVM specifies that the fcmp instruction always returns an ‘i1’ value
// (a one bit integer). The problem with this is that Kaleidoscope wants the value to be a 0.0 or 1.0 value.
// In order to get these semantics, we combine the fcmp instruction with a uitofp instruction.
// This instruction converts its input integer into a floating point value by treating the input as an unsigned value.
// In contrast, if we used the sitofp instruction, the Kaleidoscope ‘<’ operator would return 0.0 and -1.0,
// depending on the input value.
Value *BinaryExprAST::codegen()
{
    Value *L = this->LHS->codegen();
    Value *R = this->RHS->codegen();
    if (!L || !R)
    {
        return nullptr;
    }

    switch (this->Op)
    {
    case '+':
        return Builder.CreateAdd(L, R, "addtmp");
    case '-':
        return Builder.CreateSub(L, R, "subtmp");
    case '*':
        return Builder.CreateMul(L, R, "multmp");
    case '/':
        return Builder.CreateFDiv(L, R, "fdivtmp");
    case '<':
        L = Builder.CreateFCmpULT(L, R, "cmptmp");
        // Convert bool 0/1 to double 0.0 or 1.0
        return Builder.CreateUIToFP(L, Type::getDoubleTy(TheContext), "booltmp");

    default:
        return LogErrorV("invalid binary operator");
    }
}

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST
{
    // function name
    std::string Callee;
    // function args
    std::vector<std::unique_ptr<ExprAST>> Args;

  public:
    CallExprAST(const std::string &Callee,
                std::vector<std::unique_ptr<ExprAST>> Args)
        : Callee(Callee), Args(std::move(Args)) {}

    virtual Value *codegen();
};

// Code generation for function calls is quite straightforward with LLVM. The code above initially 
// does a function name lookup in the LLVM Module’s symbol table. Recall that the LLVM Module is 
// the container that holds the functions we are JIT’ing. By giving each function the same name as what 
// the user specifies, we can use the LLVM symbol table to resolve function names for us.

// Once we have the function to call, we recursively codegen each argument that is to be passed in, 
// and create an LLVM call instruction. Note that LLVM uses the native C calling conventions by default, 
// allowing these calls to also call into standard library functions like “sin” and “cos”, with no additional effort.

// This wraps up our handling of the four basic expressions that we have so far in Kaleidoscope. 
// Feel free to go in and add some more. For example, by browsing the LLVM language reference you’ll 
// find several other interesting instructions that are really easy to plug into our basic framework.
Value *CallExprAST::codegen()
{
    // Look up the name in the global module table.
    Function *CalleeF = TheModule->getFunction(this->Callee);
    if (CalleeF == nullptr)
    {
        return LogErrorV("Unknown function referenced");
    }
    // If argument mismatch error.
    if (CalleeF->arg_size() != this->Args.size())
    {
        return LogErrorV("Incorrect # arguments passed");
    }

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; i++)
    {
        // Args[i] are exprAST
        ArgsV.push_back(this->Args[i]->codegen());
        if (Args.back() == nullptr)
        {
            return nullptr;
        }
    }

    return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes).
class PrototypeAST
{
    std::string Name;
    std::vector<std::string> Args;

  public:
    PrototypeAST(const std::string &name, std::vector<std::string> Args)
        : Name(name), Args(std::move(Args)) {}

    const std::string &getName() const
    {
        return Name;
    }
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST
{
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;

  public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto, std::unique_ptr<ExprAST> Body)
        : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

// In Kaleidoscope, functions are typed with just a count of their arguments.
// Since all values are double precision floating point,
// the type of each argument doesn’t need to be stored anywhere.
// In a more aggressive and realistic language,
// the “ExprAST” class would probably have a type field.

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken()
{
    return CurTok = gettok();
}

/// BinopPrecedence - This holds the precedence for each binary operator that is
/// defined.
static std::map<char, int> BinopPrecedence;

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence()
{
    // not a binary op
    if (!isascii(CurTok))
    {
        return -1;
    }

    // Make sure it's a declared binop.
    int TokPrec = BinopPrecedence[CurTok];
    if (TokPrec <= 0)
    {
        return -1;
    }

    return TokPrec;
}

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogError(const char *Str)
{
    fprintf(stderr, "Error: %s\n", Str);
    return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char *Str)
{
    LogError(Str);
    return nullptr;
}

// TheContext is an opaque object that owns a lot of core LLVM data structures,
// such as the type and constant value tables. We don’t need to understand it in detail,
// we just need a single instance to pass into APIs that require it.
static LLVMContext TheContext;
// The Builder object is a helper object that makes it easy to generate LLVM instructions.
// Instances of the IRBuilder class template keep track of the current place to insert
// instructions and has methods to create new instructions.
static IRBuilder<> Builder(TheContext);
// TheModule is an LLVM construct that contains functions and global variables.
// In many ways, it is the top-level structure that the LLVM IR uses to contain code.
// It will own the memory for all of the IR that we generate, which is why the
// codegen() method returns a raw Value*, rather than a unique_ptr<Value>.
static std::unique_ptr<Module> TheModule;
// The NamedValues map keeps track of which values are defined in the current scope
// and what their LLVM representation is. (In other words, it is a symbol table for the code).
// In this form of Kaleidoscope, the only things that can be referenced are function parameters.
// As such, function parameters will be in this map when generating code for their function body.
static std::map<std::string, Value *> NamedValued;
// With these basics in place, we can start talking about how to generate code for each expression.
// Note that this assumes that the Builder has been set up to generate code into something.
// For now, we’ll assume that this has already been done, and we’ll just use it to emit code.

Value *LogErrorV(const char *Str)
{
    LogError(Str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr()
{
    auto Result = llvm::make_unique<NumberExprAST>(NumVal);
    getNextToken(); // consume the number
    return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr()
{
    getNextToken();             // eat '('
    auto V = ParseExpression(); // defined follow
    if (V == nullptr)
    {
        return nullptr;
    }

    if (CurTok != ')')
    {
        return LogError("expected ')'");
    }
    getNextToken(); // eat ')'
    return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr()
{
    std::string IdName = IdentifierStr;

    getNextToken(); // eat identifier.

    if (CurTok != '(') // Simple variable ref.
        return llvm::make_unique<VariableExprAST>(IdName);

    // Call.
    getNextToken(); // eat '('
    std::vector<std::unique_ptr<ExprAST>> Args;
    // Parse the args.
    if (CurTok != ')')
    {
        for (;;)
        {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if (CurTok == ')')
                break;

            if (CurTok != ',')
                return LogError("Expected ')' or ',' in argument list");
            getNextToken();
        }
    }

    // eat ')'.
    getNextToken();

    return llvm::make_unique<CallExprAST>(IdName, std::move(Args));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary()
{
    switch (CurTok)
    {
    case tok_identifier:
        return ParseIdentifierExpr();
    case tok_number:
        return ParseNumberExpr();
    case '(':
        return ParseParenExpr();
    default:
        return LogError("unknown token when expecting an expression");
    }
}

/// binoprhs
///   ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS)
{
    // If this is a binop, find its precedence.
    while (true)
    {
        int TokPrec = GetTokPrecedence();

        // If this is a binop that binds at least as tightly as the current binop,
        // consume it, otherwise we are done.

        // because we defined invalid tokens to have a precedence of
        // -1, this check implicitly knows that the pair-stream ends
        // when the token stream runs out of binary operators.
        if (TokPrec < ExprPrec)
        {
            return LHS;
        }

        // Okay, we know this is a binop.
        int BinOp = CurTok;
        getNextToken(); // eat binop

        // Parse the primary expression after the binary operator.
        auto RHS = ParsePrimary();
        if (!RHS)
        {
            return nullptr;
        }

        // If BinOp binds less tightly with RHS than the operator after RHS, let
        // the pending operator take RHS as its LHS.
        int NextPrec = GetTokPrecedence();

        // The next expr will be eval firstly.
        if (TokPrec < NextPrec)
        {
            // parse higher precedence expr by recursive firstly.
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if (!RHS)
                return nullptr;
        }

        // Merge LHS/RHS.
        LHS = llvm::make_unique<BinaryExprAST>(BinOp, std::move(LHS),
                                               std::move(RHS));
    }
}

/// expression
///   ::= primary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression()
{
    auto LHS = ParsePrimary();
    if (!LHS)
    {
        return nullptr;
    }

    return ParseBinOpRHS(0, std::move(LHS));
}

/// prototype
///   ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype()
{
    if (CurTok != tok_identifier)
    {
        return LogErrorP("Expected function name in prototype");
    }

    std::string FnName = IdentifierStr;
    getNextToken();
    if (CurTok != '(')
    {
        return LogErrorP("Expected '(' in prototype");
    }

    // Read the list of argument names.
    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
    {
        ArgNames.push_back(IdentifierStr);
    }
    if (CurTok != ')')
    {
        return LogErrorP("Expected ')' in prototype");
    }

    // eat ')'.
    getNextToken();

    return llvm::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition()
{
    getNextToken(); // eat def.
    auto Proto = ParsePrototype();
    if (!Proto)
    {
        return nullptr;
    }
    if (auto E = ParseExpression())
    {
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern()
{
    getNextToken(); // eat extern.
    return ParsePrototype();
}

/// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr()
{
    if (auto E = ParseExpression())
    {
        // Make an anonymous proto.
        auto Proto = llvm::make_unique<PrototypeAST>("__anon_expr",
                                                     std::vector<std::string>());
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing
//===----------------------------------------------------------------------===//

/// top ::= definition | dexternal | expression | ';'
static void HandleDefinition();
static void HandleExtern();
static void HandleTopLevelExpression();

static void MainLoop()
{
    for (;;)
    {
        fprintf(stderr, "ready> ");
        switch (CurTok)
        {
        case tok_eof:
            return;
        case ';': // ignore top-level semicolons.
            getNextToken();
            break;
        case tok_def:
            HandleDefinition();
            break;
        case tok_extern:
            HandleExtern();
            break;
        default:
            HandleTopLevelExpression();
            break;
        }
    }
}

static void HandleDefinition()
{
    if (ParseDefinition())
    {
        fprintf(stderr, "Parsed a function defintion.\n");
    }
    else
    {
        // Skip token for error recovery;
        getNextToken();
    }
}

static void HandleExtern()
{
    if (ParseExtern())
    {
        fprintf(stderr, "Parsed an extern\n");
    }
    else
    {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleTopLevelExpression()
{
    if (ParseTopLevelExpr())
    {
        fprintf(stderr, "Parsed a top-level expr\n");
    }
    else
    {
        // Skip token for error recovery;
        getNextToken();
    }
}

// The most interesting part of this is that we ignore top-level
// semicolons. Why is this, you ask? The basic reason is that if
// you type “4 + 5” at the command line, the parser doesn’t know
// whether that is the end of what you will type or not.
// For example, on the next line you could type “def foo…”
// in which case 4+5 is the end of a top-level expression.
// Alternatively you could type “* 6”, which would continue
// the expression. Having top-level semicolons allows you to
// type “4+5;”, and the parser will know you are done.

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main()
{
    // Install standard binary operators.
    // 1 is lowest precedence.
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 20;
    BinopPrecedence['*'] = 40; // highest.
    BinopPrecedence['/'] = 40; // highest.

    // Prime the first token.
    fprintf(stderr, "ready> ");
    getNextToken();

    // Run the main "interpreter loop" now.
    MainLoop();

    return 0;
}