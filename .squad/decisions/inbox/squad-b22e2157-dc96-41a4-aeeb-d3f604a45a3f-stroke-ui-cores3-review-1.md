## Proposal from tester
Run: squad-b22e2157-dc96-41a4-aeeb-d3f604a45a3f

PROPOSAL: Serialize all StrokeOrder UI lifecycle and click actions on one task, and perform generation validation, controller transition, and view update under one consistent lock/order rather than relying on a naked generation integer.
