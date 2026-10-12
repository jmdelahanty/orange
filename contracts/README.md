# Pinned contract files

`frame_identity_v1.json` is a byte copy of agent-contracts
`citrus-orange/frame_identity_v1.json` at 6965cc6b3affd8ac228cfac2c5c326cf818e1114
(agent-contracts is private, so CI reads this copy). Update it only together with
the agent-contracts change, and record the new commit here.

`output_names_allowlist.json` lists names `scripts/check_output_names.py` accepts
although they look like unsuffixed frame references, each with its reason.

`digest_form_v1.json` is a byte copy of agent-contracts `orange-palette/digest_form_v1.json`
(PR #90, pending merge). `digest_form_allowlist.json` lists the digest fields
`scripts/validate_output_schemas.py` still accepts in non-token form, grouped by family (A-F
in the contract), each with its reason; it only shrinks.
