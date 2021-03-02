package com.yugabyte.yw.common.swagger;


import io.swagger.model.ApiDescription;
import io.swagger.models.Model;
import io.swagger.models.Operation;
import io.swagger.models.parameters.Parameter;
import io.swagger.models.properties.Property;

import java.util.List;
import java.util.Map;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;



import io.swagger.core.filter.SwaggerSpecFilter;

public class YWSwaggerSpecFilter implements SwaggerSpecFilter {
 public static final Logger LOG = LoggerFactory.getLogger(YWSwaggerSpecFilter.class);


@Override
public boolean isOperationAllowed(
    Operation operation,
    ApiDescription api,
    Map<String, List<String>> params,
    Map<String, String> cookies,
    Map<String, List<String>> headers) { return true; }

@Override
public boolean isParamAllowed(
    Parameter parameter,
    Operation operation,
    ApiDescription api,
    Map<String, List<String>> params,
    Map<String, String> cookies,
    Map<String, List<String>> headers) { return true; }

@Override
public boolean isPropertyAllowed(
    Model model,
    Property property,
    String propertyName,
    Map<String, List<String>> params,
    Map<String, String> cookies,
    Map<String, List<String>> headers) {
        //LOG.info(propertyName);
        if (propertyName.indexOf("_ebean_intercept") >= 0) {
            return false;
        }
        return true;
    }
}
