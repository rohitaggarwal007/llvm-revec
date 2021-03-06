// RUN: not llvm-mc -triple=aarch64 -show-encoding -mattr=+sve  2>&1 < %s| FileCheck %s

// ------------------------------------------------------------------------- //
// Invalid result register

sqinch w0
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch w0
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch wsp
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch wsp
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch sp
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch sp
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch z0.s
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid element width
// CHECK-NEXT: sqinch z0.s
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:


// ------------------------------------------------------------------------- //
// Operands not matching up

sqinch x0, w1
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: operand must be 32-bit form of destination register
// CHECK-NEXT: sqinch x0, w1
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, x0
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch x0, x0
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:


// ------------------------------------------------------------------------- //
// Immediate not compatible with encode/decode function.

sqinch x0, all, mul #-1
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: immediate must be an integer in range [1, 16]
// CHECK-NEXT: sqinch x0, all, mul #-1
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, all, mul #0
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: immediate must be an integer in range [1, 16]
// CHECK-NEXT: sqinch x0, all, mul #0
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, all, mul #17
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: immediate must be an integer in range [1, 16]
// CHECK-NEXT: sqinch x0, all, mul #17
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:


// ------------------------------------------------------------------------- //
// Invalid predicate patterns

sqinch x0, vl512
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch x0, vl512
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, vl9
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand
// CHECK-NEXT: sqinch x0, vl9
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, #-1
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid predicate pattern
// CHECK-NEXT: sqinch x0, #-1
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

sqinch x0, #32
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid predicate pattern
// CHECK-NEXT: sqinch x0, #32
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:
