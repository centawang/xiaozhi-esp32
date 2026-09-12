## Proposal from backend
Run: squad-b5cf2513-95db-454e-8fdb-3cf0bb37ea67

Propose capturing STT intercept on the network thread with the current session token, then re-validating that token under the display lock on main so a newly started stroke session cannot steal an already-copied ordinary STT, and a cancelled session cannot apply a late stroke STT.
