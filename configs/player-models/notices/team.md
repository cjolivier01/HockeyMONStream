NVIDIA ReIdentificationNet deployable_v1.2, deployed for team affiliation

This is the same unmodified ONNX as `notices/reid.md`, downloaded directly from
NVIDIA NGC and not mirrored in the HockeyMONStream release. NVIDIA's model terms
govern its use. Only the deployment differs, which is why there is a second
contract rather than a shared one.

Model card: https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/reidentificationnet
Model terms: https://www.nvidia.com/en-us/data-center/products/nvidia-ai-enterprise/eula/

Official ONNX:
https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx
SHA256: 0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b

Deployment profile:
RGB NCHW float32 input [batch,3,256,128], bilinear resize without preserving
aspect ratio, offsets [123.675,116.28,103.53], scale 0.01735207357279195, and
the unnormalized 256-component fc_pred output. Maximum batch is 8, matching the
other player-analytics features, not the tracker's 32: these crops are cut from
the chest band of due tracks within one frame's inference budget.

What the model is used for here:

The network was trained to tell individuals apart on Market-1501 and AI City
Challenge pedestrian data. Nothing in it knows about hockey, teams, or sweaters.
Team affiliation comes from clustering its embeddings per game, so the model
contributes an appearance vector and the two-means fit in
`src/libs/player_analytics/TeamAffiliation.cpp` contributes the split. "Home"
and "away" are then names applied from the game's own `home_is_dark` setting,
not anything the model predicts.

The choice of this network over a team-trained alternative was measured, not
assumed. Against PRTReid -- the SoccerNet Game State Reconstruction baseline,
whose embedding was multi-task trained against team labels -- it scored 89.5%
versus 86.8% on the same hand-labelled hockey crops, and ran 1.34x faster
through the same runtime. Separating individuals turns out to separate squads
well enough, and this model is already licensed and hosted for the tracker.

Neither that comparison nor the conversion checks establish accuracy on any
particular rink. Lighting, sweater similarity and crop quality all move the
number, and the clusterer declines to assign an embedding it cannot call.
