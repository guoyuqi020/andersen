#include "NodeFactory.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Given a GEP insn or GEP const expr, compute its byte-offset
// The function will resolve nested GEP constexpr, but will not resolve nested GEP instruction
static unsigned getGEPOffset(const Value* value, const DataLayout* dataLayout)
{
	// Assume this function always receives GEP value
	const GEPOperator* gepValue = dyn_cast<GEPOperator>(value);
	assert(gepValue != NULL && "getGEPOffset receives a non-gep value!");
	assert(dataLayout != nullptr && "getGEPOffset receives a NULL dataLayout!");

	unsigned offset = 0;
	const Value* baseValue = gepValue->getPointerOperand()->stripPointerCasts();
	// If we have yet another nested GEP const expr, accumulate its offset
	// The reason why we don't accumulate nested GEP instruction's offset is that we aren't required to. Also, it is impossible to do that because we are not sure if the indices of a GEP instruction contains all-consts or not.
	if (const ConstantExpr* cexp = dyn_cast<ConstantExpr>(baseValue))
		if (cexp->getOpcode() == Instruction::GetElementPtr)
			offset += getGEPOffset(cexp, dataLayout);

	//errs() << "gepValue = " << *gepValue << "\n";
	SmallVector<Value*, 4> indexOps(gepValue->op_begin() + 1, gepValue->op_end());
	// Make sure all indices are constants
	for (unsigned i = 0, e = indexOps.size(); i != e; ++i)
		assert(isa<ConstantInt>(indexOps[i]) && "getGEPOffset does not accept non-const GEP indices!");

	offset += dataLayout->getIndexedOffset(gepValue->getPointerOperand()->getType(), indexOps);

	return offset;
}

AndersNodeFactory::AndersNodeFactory(): dataLayout(NULL)
{
	// Note that we can't use std::vector::emplace_back() here because AndersNode's constructors are private hence std::vector cannot see it

	// Node #0 is always the universal ptr: the ptr that we don't know anything about.
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, 0));
	// Node #0 is always the universal obj: the obj that we don't know anything about.
	nodes.push_back(AndersNode(AndersNode::OBJ_NODE, 1));
	// Node #2 always represents the null pointer.
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, 2));
	// Node #3 is the object that null pointer points to
	nodes.push_back(AndersNode(AndersNode::OBJ_NODE, 3));
	// Node #4 represents all pointers casted to int
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, 4));

	assert(nodes.size() == 5);
}

NodeIndex AndersNodeFactory::createValueNode(const Value* val)
{
	unsigned nextIdx = nodes.size();
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, nextIdx, val));
	if (val != nullptr)
	{
		assert(!valueNodeMap.count(val) && "Trying to insert two mappings to revValueNodeMap!");
		valueNodeMap[val] = nextIdx;
	}

	return nextIdx;
}

NodeIndex AndersNodeFactory::createObjectNode(const Value* val)
{
	unsigned nextIdx = nodes.size();
	nodes.push_back(AndersNode(AndersNode::OBJ_NODE, nextIdx, val));
	if (val != nullptr)
	{
		assert(!objNodeMap.count(val) && "Trying to insert two mappings to revObjNodeMap!");
		objNodeMap[val] = nextIdx;
	}

	return nextIdx;
}

NodeIndex AndersNodeFactory::createReturnNode(const llvm::Function* f)
{
	unsigned nextIdx = nodes.size();
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, nextIdx, f));

	assert(!returnMap.count(f) && "Trying to insert two mappings to returnMap!");
	returnMap[f] = nextIdx;

	return nextIdx;
}

NodeIndex AndersNodeFactory::createVarargNode(const llvm::Function* f)
{
	unsigned nextIdx = nodes.size();
	nodes.push_back(AndersNode(AndersNode::VALUE_NODE, nextIdx, f));

	assert(!varargMap.count(f) && "Trying to insert two mappings to varargMap!");
	varargMap[f] = nextIdx;

	return nextIdx;
}

NodeIndex AndersNodeFactory::getValueNodeFor(const Value* val)
{
	if (const Constant* c = dyn_cast<Constant>(val))
		if (!isa<GlobalValue>(c))
			return getValueNodeForConstant(c);

	auto itr = valueNodeMap.find(val);
	if (itr == valueNodeMap.end())
		return InvalidIndex;
	else
		return itr->second;
}

NodeIndex AndersNodeFactory::getValueNodeForConstant(const llvm::Constant* c)
{
	assert(isa<PointerType>(c->getType()) && "Not a constant pointer!");
	
	if (isa<ConstantPointerNull>(c) || isa<UndefValue>(c))
    	return getNullPtrNode();
    else if (const GlobalValue* gv = dyn_cast<GlobalValue>(c))
    	return getValueNodeFor(gv);
    else if (const ConstantExpr* ce = dyn_cast<ConstantExpr>(c))
    {
		switch (ce->getOpcode())
		{
			case Instruction::GetElementPtr:
				assert(false && "This is wrong: avoid getting here!");
				return getValueNodeForConstant(ce->getOperand(0));
			case Instruction::IntToPtr:
				return getUniversalPtrNode();
			case Instruction::PtrToInt:
				return getIntPtrNode();
			case Instruction::BitCast:
				return getValueNodeForConstant(ce->getOperand(0));
			default:
				errs() << "Constant Expr not yet handled: " << *ce << "\n";
				llvm_unreachable(0);
		}
	}

	llvm_unreachable("Unknown constant pointer!");
	return InvalidIndex;
}

NodeIndex AndersNodeFactory::getObjectNodeFor(const Value* val)
{
	if (const Constant* c = dyn_cast<Constant>(val))
		if (!isa<GlobalValue>(c))
			return getObjectNodeForConstant(c);

	auto itr = objNodeMap.find(val);
	if (itr == objNodeMap.end())
		return InvalidIndex;
	else
		return itr->second;
}

NodeIndex AndersNodeFactory::getObjectNodeForConstant(const llvm::Constant* c)
{
	assert(isa<PointerType>(c->getType()) && "Not a constant pointer!");

	if (isa<ConstantPointerNull>(c))
		return getNullObjectNode();
	else if (const GlobalValue* gv = dyn_cast<GlobalValue>(c))
		return getObjectNodeFor(gv);
	else if (const ConstantExpr* ce = dyn_cast<ConstantExpr>(c))
	{
		switch (ce->getOpcode())
		{
			case Instruction::GetElementPtr:
			{
				NodeIndex baseNode = getObjectNodeForConstant(ce->getOperand(0));
				if (baseNode == getNullObjectNode() || baseNode == getUniversalObjNode())
					return baseNode;

				return getOffsetObjectNode(baseNode, constGEPtoFieldNum(ce));
			}
			case Instruction::IntToPtr:
				return getUniversalObjNode();
			case Instruction::BitCast:
				return getObjectNodeForConstant(ce->getOperand(0));
			default:
				errs() << "Constant Expr not yet handled: " << *ce << "\n";
				llvm_unreachable(0);
		}
	}

	llvm_unreachable("Unknown constant pointer!");
	return InvalidIndex;
}

NodeIndex AndersNodeFactory::getReturnNodeFor(const llvm::Function* f)
{
	auto itr = returnMap.find(f);
	if (itr == returnMap.end())
		return InvalidIndex;
	else
		return itr->second;
}

NodeIndex AndersNodeFactory::getVarargNodeFor(const llvm::Function* f)
{
	auto itr = varargMap.find(f);
	if (itr == varargMap.end())
		return InvalidIndex;
	else
		return itr->second;
}

unsigned AndersNodeFactory::constGEPtoFieldNum(const llvm::ConstantExpr* expr) const
{
	assert(expr->getOpcode() == Instruction::GetElementPtr && "constGEPtoVariable receives a non-gep expr!");

	unsigned offset = getGEPOffset(expr, dataLayout);
	return offsetToFieldNum(GetUnderlyingObject(expr, dataLayout, 0), offset);
}

unsigned AndersNodeFactory::offsetToFieldNum(const Value* ptr, unsigned offset) const
{
	assert(ptr->getType()->isPointerTy() && "Passing a non-ptr to offsetToFieldNum!");
	assert(dataLayout != nullptr && "DataLayout is NULL when calling offsetToFieldNum!");

	Type* trueElemType = cast<PointerType>(ptr->getType())->getElementType();
	unsigned ret = 0;
	while (offset > 0)
	{
		// Collapse array type
		while(const ArrayType *arrayType= dyn_cast<ArrayType>(trueElemType))
			trueElemType = arrayType->getElementType();

		//errs() << "trueElemType = "; trueElemType->dump(); errs() << "\n";
		offset %= dataLayout->getTypeAllocSize(trueElemType);
		if (trueElemType->isStructTy())
		{
			StructType* stType = cast<StructType>(trueElemType);
			const StructLayout* stLayout = dataLayout->getStructLayout(stType);
			unsigned idx = stLayout->getElementContainingOffset(offset);
			const StructInfo* stInfo = structAnalyzer->getStructInfo(stType);
			assert(stInfo != NULL && "structInfoMap should have info for all structs!");
			
			ret += stInfo->getOffset(idx);
			offset -= stLayout->getElementOffset(idx);
			trueElemType = stType->getElementType(idx);
		}
		else
		{
			if (offset != 0)
			{
				errs() << "Warning: GEP into the middle of a field. This usually occurs when union is used. Since partial alias is not supported, correctness is not guanranteed here.\n";
				break;
			}
		}
	}
	return ret;
}

void AndersNodeFactory::dumpNode(NodeIndex idx) const
{
	const AndersNode& n = nodes.at(idx);
	if (n.type == AndersNode::VALUE_NODE)
		errs() << "[V ";
	else if (n.type == AndersNode::OBJ_NODE)
		errs() << "[O ";
	else
		assert(false && "Wrong type number!");
	errs() << "#" << n.idx << "]";
}

void AndersNodeFactory::dumpNodeInfo() const
{
	errs() << "\n----- Print AndersNodeFactory Info -----\n";
	for (auto const& node: nodes)
	{
		dumpNode(node.getIndex());
		errs() << ", val = ";
		const Value* val = node.getValue();
		if (val == nullptr)
			errs() << "NULL";
		else if (isa<Function>(val))
			errs() << "  <func> " << val->getName();
		else
			errs() << val->getName();
		errs() << "\n";
	}

	errs() << "\nReturn Map:\n";
	for (auto const& mapping: returnMap)
		errs() << mapping.first->getName() << "  -->>  [Node #" << mapping.second << "]\n";
	errs() << "\nVararg Map:\n";
	for (auto const& mapping: varargMap)
		errs() << mapping.first->getName() << "  -->>  [Node #" << mapping.second << "]\n";
	errs() << "----- End of Print -----\n";
}
