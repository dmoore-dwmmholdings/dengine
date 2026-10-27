
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

    for (std::size_t contourIndex = 0; contourIndex < shape.contours.size(); ++contourIndex) {
        const Contour *contour = shape.contours.data()+contourIndex;
        if (!contour->edges.empty()) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contourIndex));

            const EdgeSegment *prevEdge = contour->edges.size() >= 2 ? contour->edges.data()[contour->edges.size()-2] : contour->edges.data()[0];
            const EdgeSegment *curEdge = contour->edges.back();
            for (std::size_t edgeIndex = 0; edgeIndex < contour->edges.size(); ++edgeIndex) {
                const EdgeSegment *nextEdge = contour->edges.data()[edgeIndex];
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

    for (std::size_t contourIndex = 0; contourIndex < shape.contours.size(); ++contourIndex) {
        const Contour *contour = shape.contours.data()+contourIndex;
        if (!contour->edges.empty()) {
            typename ContourCombiner::EdgeSelectorType &edgeSelector = contourCombiner.edgeSelector(int(contourIndex));

            const EdgeSegment *prevEdge = contour->edges.size() >= 2 ? contour->edges.data()[contour->edges.size()-2] : contour->edges.data()[0];
            const EdgeSegment *curEdge = contour->edges.back();
            for (std::size_t edgeIndex = 0; edgeIndex < contour->edges.size(); ++edgeIndex) {
                const EdgeSegment *nextEdge = contour->edges.data()[edgeIndex];
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
