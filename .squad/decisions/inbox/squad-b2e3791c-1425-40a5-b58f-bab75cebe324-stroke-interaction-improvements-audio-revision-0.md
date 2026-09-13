## Proposal from backend
Run: squad-b2e3791c-1425-40a5-b58f-bab75cebe324

Propose keeping worker join in destructor and Start-after-Stop only (portMAX_DELAY on EXITED bits), not inside Stop(), so normal main/audio paths stay non-blocking; document that a worker stuck in codec I/O can delay teardown.
