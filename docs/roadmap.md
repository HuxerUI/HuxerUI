# Roadmap

This roadmap records intended capability areas, not committed release dates.
Current behavior is documented in the user guides, while detailed constraints live in the design reference.

## Accessibility

- Add platform accessibility bridges for Linux and Web.
- Expand platform text-editing and collection mappings where the shared semantic contract already provides the required data.
- Keep accessibility behavior derived from one shared `SemanticFrame` rather than platform-specific component logic.

## Platform integration

- Add Linux `PlatformView` hosting while preserving shared composition ordering.
- Complete iOS archive export, distribution signing, and embeddable host integration.
- Add an OHOS backend through the existing platform boundaries.

## Interaction and animation

- Add ordinary View lifecycle transitions without introducing per-component Runtime branches.
- Expand paint primitives only when a production component or application requires them.

## Navigation and restoration

- Define saveable application and navigation state without making factory-only routes serializable by implication.
- Add typed navigation results and richer adaptive navigation only when they preserve the existing single path of truth.
- Extend platform activation and deep-link integration while keeping URL and route policy application-owned.

## Resources and distribution

- Add localized image selection, inherited locale-aware text shaping, and explicit resource preload policy.
- Publish signed platform packages where the platform ecosystem requires them.
- Extend SDK packaging and signing without duplicating build policy in generated projects.

Implementation proposals must update the relevant design document before changing public API or ownership boundaries.
