class ConditionalNode extends ControlNode {
    
    ConditionalNode(ExpressionNode aCondition, ControlNode aTrueCode)
    {
        super(aCondition);
        trueCode = aTrueCode;
    }
    
    ConditionalNode(ExpressionNode aCondition, ControlNode aTrueCode, ControlNode aFalseCode)
    {
        super(aCondition);
        trueCode = aTrueCode;
        setNext(aFalseCode);
    }
    
    ControlNode eval(Environment theEnv)
    {
        ControlNode n = super.eval(theEnv);
        double d = theEnv.theStack.pop().d;
        if (d != 0.0)
            return trueCode;
        else
            return n;
    }

    String print()
    {
        StringBuffer result = new StringBuffer("ConditionalNode ");
        result.append(index);
        result.append("\nexpr:\n");
        if (expr == null)
            result.append("expr = null\n");
        else
            result.append(expr.print(""));
        result.append("true branch:\n");
        if (trueCode == null)
            result.append("true branch = null\n");
        else
            result.append("true branch->" + trueCode.index + "\n");
        result.append("false branch:\n");
        if (next == null)
            result.append("false branch = null\n");
        else
            result.append("false branch->" + next.index + "\n");
        return result.toString();
    }
 
    protected ControlNode trueCode;
}
