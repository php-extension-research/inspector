--TEST--
InspectorOperand operand type
--FILE--
<?php
use Inspector\InspectorFunction;
use Inspector\InspectorBreakPoint;
use Inspector\InspectorFrame;
use Inspector\InspectorOperand;
use Inspector\InspectorInstruction;

function foo($a, $b) {
	$a + $b;
};

$inspector = new InspectorFunction("foo");

$opline = $inspector->findFirstInstruction(InspectorInstruction::ZEND_ADD);

$op1 = $opline->getOperand(InspectorOperand::OP1);

if ($op1->isCompiledVariable() &&
    !$op1->isTemporaryVariable() &&
    !$op1->isVariable() &&
    !$op1->isConstant()) {
	echo "OK";
}
?>
--EXPECT--
OK
