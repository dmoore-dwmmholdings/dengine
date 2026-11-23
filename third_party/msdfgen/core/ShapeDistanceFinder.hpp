
#include "ShapeDistanceFinder.h"

namespace msdfgen {

template <class ContourCombiner>
ShapeDistanceFinder<ContourCombiner>::ShapeDistanceFinder(const Shape &shape) : shape(shape), contourCombiner(shape), shapeEdgeCache(shape.edgeCount()) { }

template <class ContourCombiner>
typename ShapeDistanceFinder<ContourCombiner>::DistanceType ShapeDistanceFinder<ContourCombiner>::distance(const Point2 &origin) {
    contourCombiner.reset(origin);
#ifdef MSDFGEN_USE_CPP11
    typename ContourCombiner::EdgeSelectorType::EdgeCache *edgeCache = shapeEdgeCache.data();
#else
    typename ContourCombiner::EdgeSelectorType::EdgeCache *edgeCache = shapeEdgeCache.empty() ? NULL : &shapeEdgeCache[0];
#endif

    // Pointer walks (DENG-758): checked-iterator builds lock a global mutex per iterator.
    const Contour *contours = shape.contours.empty() ? NULL : &shape.contours[0];
    for (const Contour *contour = contours, *contourEnd = contours+shape.contours.size(); contour != contourEnd; ++contour) {
        if (!contour->edges.empty()) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contour-contours));

            const EdgeHolder *edges = &contour->edges[0], *edgesEnd = edges+contour->edges.size();
            const EdgeSegment *prevEdge = contour->edges.size() >= 2 ? *(edgesEnd-2) : *edges;
            const EdgeSegment *curEdge = *(edgesEnd-1);
            for (const EdgeHolder *edge = edges; edge != edgesEnd; ++edge) {
                const EdgeSegment *nextEdge = *edge;
                edgeSelector.addEdge(*edgeCache++, prevEdge, curEdge, nextEdge);
                prevEdge = curEdge;
                curEdge = nextEdge;
            }
        }
    }

    return contourCombiner.distance();
}

template <class ContourCombiner>
typename ShapeDistanceFinder<ContourCombiner>::DistanceType ShapeDistanceFinder<ContourCombiner>::oneShotDistance(const Shape &shape, const Point2 &origin) {
    ContourCombiner contourCombiner(shape);
    contourCombiner.reset(origin);

    // Pointer walks (DENG-758): checked-iterator builds lock a global mutex per iterator.
    const Contour *contours = shape.contours.empty() ? NULL : &shape.contours[0];
    for (const Contour *contour = contours, *contourEnd = contours+shape.contours.size(); contour != contourEnd; ++contour) {
        if (!contour->edges.empty()) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contour-contours));

            const EdgeHolder *edges = &contour->edges[0], *edgesEnd = edges+contour->edges.size();
            const EdgeSegment *prevEdge = contour->edges.size() >= 2 ? *(edgesEnd-2) : *edges;
            const EdgeSegment *curEdge = *(edgesEnd-1);
            for (const EdgeHolder *edge = edges; edge != edgesEnd; ++edge) {
                const EdgeSegment *nextEdge = *edge;
                typename ContourCombiner::EdgeSelectorType::EdgeCache dummy;
                edgeSelector.addEdge(dummy, prevEdge, curEdge, nextEdge);
                prevEdge = curEdge;
                curEdge = nextEdge;
            }
        }
    }

    return contourCombiner.distance();
}

}
