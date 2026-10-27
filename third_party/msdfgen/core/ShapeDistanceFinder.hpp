
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

    const Contour *contours = shape.contours.data();
    const std::size_t contourCount = shape.contours.size();
    for (std::size_t contourIndex = 0; contourIndex < contourCount; ++contourIndex) {
        const Contour *contour = contours+contourIndex;
        const EdgeHolder *edges = contour->edges.data();
        const std::size_t edgeCount = contour->edges.size();
        if (edgeCount) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contourIndex));

            const EdgeSegment *prevEdge = edges[edgeCount >= 2 ? edgeCount-2 : 0];
            const EdgeSegment *curEdge = edges[edgeCount-1];
            for (std::size_t edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex) {
                const EdgeSegment *nextEdge = edges[edgeIndex];
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

    const Contour *contours = shape.contours.data();
    const std::size_t contourCount = shape.contours.size();
    for (std::size_t contourIndex = 0; contourIndex < contourCount; ++contourIndex) {
        const Contour *contour = contours+contourIndex;
        const EdgeHolder *edges = contour->edges.data();
        const std::size_t edgeCount = contour->edges.size();
        if (edgeCount) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contourIndex));

            const EdgeSegment *prevEdge = edges[edgeCount >= 2 ? edgeCount-2 : 0];
            const EdgeSegment *curEdge = edges[edgeCount-1];
            for (std::size_t edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex) {
                const EdgeSegment *nextEdge = edges[edgeIndex];
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
